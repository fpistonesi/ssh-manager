#include "vault.h"

#include "crypto.h"
#include "paths.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <openssl/evp.h>

namespace {

constexpr char kMagic[] = "SSHManagerVault1";
constexpr int kMagicSize = 16;
constexpr int kNonceSize = 12;
constexpr int kTagSize = 16;
constexpr int kSaltSize = 16;
constexpr quint32 kDefaultIterations = 600000;
constexpr int kMaxHeaderSize = 16384;

using uchar = unsigned char;
const uchar *u(const QByteArray &data)
{
    return reinterpret_cast<const uchar *>(data.constData());
}
uchar *u(QByteArray &data)
{
    return reinterpret_cast<uchar *>(data.data());
}

QByteArray deriveKey(const QByteArray &password, const QByteArray &salt, quint32 iterations)
{
    QByteArray key(32, '\0');
    if (PKCS5_PBKDF2_HMAC(password.constData(), password.size(), u(salt), salt.size(),
                          int(iterations), EVP_sha256(), key.size(), u(key))
        != 1) {
        crypto::cleanse(key);
        return QByteArray();
    }
    return key;
}

void appendU32(QByteArray &out, quint32 value)
{
    out.append(char(value & 0xff));
    out.append(char((value >> 8) & 0xff));
    out.append(char((value >> 16) & 0xff));
    out.append(char((value >> 24) & 0xff));
}

bool readU32(const QByteArray &data, int offset, quint32 *value)
{
    if (offset < 0 || offset + 4 > data.size())
        return false;
    *value = quint32(quint8(data.at(offset)))
             | (quint32(quint8(data.at(offset + 1))) << 8)
             | (quint32(quint8(data.at(offset + 2))) << 16)
             | (quint32(quint8(data.at(offset + 3))) << 24);
    return true;
}

bool aesEncrypt(const QByteArray &key, const QByteArray &nonce, const QByteArray &aad,
                const QByteArray &plain, QByteArray *cipher, QByteArray *tag)
{
    cipher->assign(plain.size() + 16, '\0');
    tag->assign(kTagSize, '\0');

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    bool ok = false;
    int total = 0;
    int len = 0;
    if (ctx && EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) == 1
        && EVP_EncryptInit_ex(ctx, nullptr, nullptr, u(key), u(nonce)) == 1) {
        if (!aad.isEmpty())
            EVP_EncryptUpdate(ctx, nullptr, &len, u(aad), aad.size());
        if (EVP_EncryptUpdate(ctx, u(*cipher), &len, u(plain), plain.size()) == 1) {
            total = len;
            if (EVP_EncryptFinal_ex(ctx, u(*cipher) + total, &len) == 1) {
                total += len;
                ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagSize, tag->data()) == 1;
            }
        }
    }
    if (ctx)
        EVP_CIPHER_CTX_free(ctx);

    cipher->truncate(total);
    if (!ok) {
        crypto::cleanse(*cipher);
        crypto::cleanse(*tag);
    }
    return ok;
}

bool aesDecrypt(const QByteArray &key, const QByteArray &nonce, const QByteArray &aad,
                const QByteArray &tag, const QByteArray &cipher, QByteArray *plain)
{
    plain->assign(cipher.size() + 16, '\0');
    QByteArray tagCopy = tag;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    bool ok = false;
    int total = 0;
    int len = 0;
    if (ctx && EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) == 1
        && EVP_DecryptInit_ex(ctx, nullptr, nullptr, u(key), u(nonce)) == 1) {
        if (!aad.isEmpty())
            EVP_DecryptUpdate(ctx, nullptr, &len, u(aad), aad.size());
        if (EVP_DecryptUpdate(ctx, u(*plain), &len, u(cipher), cipher.size()) == 1) {
            total = len;
            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagSize, tagCopy.data()) == 1
                && EVP_DecryptFinal_ex(ctx, u(*plain) + total, &len) == 1) {
                total += len;
                ok = true;
            }
        }
    }
    if (ctx)
        EVP_CIPHER_CTX_free(ctx);

    plain->truncate(total);
    crypto::cleanse(tagCopy);
    if (!ok)
        crypto::cleanse(*plain);
    return ok;
}

bool writeBlob(const QString &path, const QByteArray &key, const QByteArray &salt,
               quint32 iterations, const QByteArray &payload, QString *error)
{
    if (key.size() != 32 || salt.isEmpty()) {
        *error = QStringLiteral("L'archivio è bloccato.");
        return false;
    }

    QJsonObject info;
    info[QStringLiteral("v")] = 1;
    info[QStringLiteral("salt")] = QString::fromLatin1(salt.toBase64());
    info[QStringLiteral("iter")] = int(iterations);

    const QByteArray headerJson = QJsonDocument(info).toJson(QJsonDocument::Compact);
    QByteArray aad(kMagic, kMagicSize);
    aad.append(headerJson);

    const QByteArray nonce = crypto::randomBytes(kNonceSize);
    QByteArray cipher;
    QByteArray tag;
    if (!aesEncrypt(key, nonce, aad, payload, &cipher, &tag)) {
        *error = QStringLiteral("Cifratura dell'archivio non riuscita.");
        return false;
    }

    QByteArray blob(kMagic, kMagicSize);
    appendU32(blob, quint32(headerJson.size()));
    blob.append(headerJson);
    appendU32(blob, quint32(nonce.size()));
    blob.append(nonce);
    blob.append(tag);
    blob.append(cipher);

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        *error = QStringLiteral("Impossibile scrivere %1.").arg(path);
        return false;
    }
    file.write(blob);
    if (!file.commit()) {
        *error = QStringLiteral("Scrittura dell'archivio non riuscita.");
        return false;
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

} // namespace

Vault::Vault(QString path) : m_path(path.isEmpty() ? paths::vaultFile() : std::move(path)) {}

Vault::~Vault()
{
    lock();
}

bool Vault::exists() const
{
    // An empty file counts as missing: creating an empty vault.db next to the
    // executable is how a new portable vault is started.
    const QFileInfo info(m_path);
    return info.exists() && info.size() > 0;
}

bool Vault::create(const QByteArray &password, const QByteArray &payload, QString *error)
{
    if (exists()) {
        *error = QStringLiteral("Esiste già un archivio in %1.").arg(m_path);
        return false;
    }
    if (password.size() < 8) {
        *error = QStringLiteral("La master password deve contenere almeno 8 caratteri.");
        return false;
    }

    crypto::cleanse(m_key);
    m_salt = crypto::randomBytes(kSaltSize);
    m_iterations = kDefaultIterations;
    m_key = deriveKey(password, m_salt, m_iterations);
    if (m_key.size() != 32) {
        *error = QStringLiteral("Derivazione della chiave non riuscita.");
        lock();
        return false;
    }

    m_unlocked = true;
    if (!writePayload(payload, error)) {
        lock();
        return false;
    }
    return true;
}

bool Vault::unlock(const QByteArray &password, QByteArray *payload, QString *error)
{
    lock();

    QByteArray header;
    QByteArray nonce;
    QByteArray tag;
    QByteArray cipher;
    if (!readFile(&header, &nonce, &tag, &cipher, error))
        return false;

    const QJsonObject info = QJsonDocument::fromJson(header).object();
    const QByteArray salt = QByteArray::fromBase64(info.value(QStringLiteral("salt")).toString().toLatin1());
    const quint32 iterations = quint32(info.value(QStringLiteral("iter")).toInt(int(kDefaultIterations)));
    if (salt.size() < 8 || iterations < 10000) {
        *error = QStringLiteral("Archivio danneggiato o non riconosciuto.");
        return false;
    }

    QByteArray key = deriveKey(password, salt, iterations);
    if (key.size() != 32) {
        *error = QStringLiteral("Derivazione della chiave non riuscita.");
        return false;
    }

    QByteArray aad(kMagic, kMagicSize);
    aad.append(header);

    QByteArray plain;
    const bool ok = aesDecrypt(key, nonce, aad, tag, cipher, &plain);
    if (!ok) {
        crypto::cleanse(key);
        *error = QStringLiteral("Master password errata o archivio danneggiato.");
        return false;
    }

    m_key = key;
    m_salt = salt;
    m_iterations = iterations;
    m_unlocked = true;
    *payload = plain;
    return true;
}

bool Vault::save(const QByteArray &payload, QString *error)
{
    if (!m_unlocked) {
        *error = QStringLiteral("L'archivio è bloccato.");
        return false;
    }
    return writePayload(payload, error);
}

bool Vault::changePassword(const QByteArray &currentPassword, const QByteArray &newPassword,
                           const QByteArray &payload, QString *error)
{
    if (!m_unlocked) {
        *error = QStringLiteral("L'archivio è bloccato.");
        return false;
    }
    if (newPassword.size() < 8) {
        *error = QStringLiteral("La nuova master password deve contenere almeno 8 caratteri.");
        return false;
    }

    QByteArray header;
    QByteArray nonce;
    QByteArray tag;
    QByteArray cipher;
    if (!readFile(&header, &nonce, &tag, &cipher, error))
        return false;

    const QJsonObject info = QJsonDocument::fromJson(header).object();
    const QByteArray fileSalt =
        QByteArray::fromBase64(info.value(QStringLiteral("salt")).toString().toLatin1());
    const quint32 iterations = quint32(info.value(QStringLiteral("iter")).toInt(int(kDefaultIterations)));
    if (fileSalt.size() < 8 || iterations < 10000) {
        *error = QStringLiteral("Archivio danneggiato o non riconosciuto.");
        return false;
    }

    QByteArray aad(kMagic, kMagicSize);
    aad.append(header);

    QByteArray verifyKey = deriveKey(currentPassword, fileSalt, iterations);
    QByteArray verified;
    const bool matches = verifyKey.size() == 32
                         && aesDecrypt(verifyKey, nonce, aad, tag, cipher, &verified);
    crypto::cleanse(verifyKey);
    crypto::cleanse(verified);
    if (!matches) {
        *error = QStringLiteral("Master password attuale non corretta.");
        return false;
    }

    const QByteArray newSalt = crypto::randomBytes(kSaltSize);
    QByteArray newKey = deriveKey(newPassword, newSalt, kDefaultIterations);
    if (newKey.size() != 32) {
        *error = QStringLiteral("Derivazione della chiave non riuscita.");
        return false;
    }

    QByteArray previousKey = m_key;
    QByteArray previousSalt = m_salt;
    const quint32 previousIterations = m_iterations;

    m_key = newKey;
    m_salt = newSalt;
    m_iterations = kDefaultIterations;

    if (!writePayload(payload, error)) {
        m_key = previousKey;
        m_salt = previousSalt;
        m_iterations = previousIterations;
        return false;
    }

    crypto::cleanse(previousKey);
    return true;
}

void Vault::lock()
{
    crypto::cleanse(m_key);
    m_salt.clear();
    m_unlocked = false;
}

bool Vault::writePayload(const QByteArray &payload, QString *error)
{
    if (!m_unlocked || m_key.size() != 32 || m_salt.isEmpty()) {
        *error = QStringLiteral("L'archivio è bloccato.");
        return false;
    }
    return writeBlob(m_path, m_key, m_salt, m_iterations, payload, error);
}

bool Vault::writeArchive(const QString &destination, const QByteArray &password,
                         const QByteArray &payload, QString *error)
{
    if (password.size() < 8) {
        *error = QStringLiteral("La password deve contenere almeno 8 caratteri.");
        return false;
    }

    const QByteArray salt = crypto::randomBytes(kSaltSize);
    QByteArray key = deriveKey(password, salt, kDefaultIterations);
    if (key.size() != 32) {
        *error = QStringLiteral("Derivazione della chiave non riuscita.");
        return false;
    }

    const bool ok = writeBlob(destination, key, salt, kDefaultIterations, payload, error);
    crypto::cleanse(key);
    return ok;
}

bool Vault::readFile(QByteArray *header, QByteArray *nonce, QByteArray *tag, QByteArray *cipher,
                     QString *error) const
{
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Impossibile aprire l'archivio %1.").arg(m_path);
        return false;
    }
    const QByteArray blob = file.readAll();
    if (blob.size() < kMagicSize + 8 || !blob.startsWith(QByteArray(kMagic, kMagicSize))) {
        *error = QStringLiteral("Archivio non valido.");
        return false;
    }

    int offset = kMagicSize;
    quint32 headerLength = 0;
    if (!readU32(blob, offset, &headerLength)) {
        *error = QStringLiteral("Archivio non valido.");
        return false;
    }
    offset += 4;
    if (headerLength == 0 || headerLength > quint32(kMaxHeaderSize)
        || offset + int(headerLength) + 4 > blob.size()) {
        *error = QStringLiteral("Archivio non valido.");
        return false;
    }
    *header = blob.mid(offset, int(headerLength));
    offset += int(headerLength);

    quint32 nonceLength = 0;
    if (!readU32(blob, offset, &nonceLength)) {
        *error = QStringLiteral("Archivio non valido.");
        return false;
    }
    offset += 4;
    if (nonceLength != quint32(kNonceSize) || offset + int(nonceLength) + kTagSize > blob.size()) {
        *error = QStringLiteral("Archivio non valido.");
        return false;
    }
    *nonce = blob.mid(offset, int(nonceLength));
    offset += int(nonceLength);
    *tag = blob.mid(offset, kTagSize);
    offset += kTagSize;
    *cipher = blob.mid(offset);
    return true;
}
