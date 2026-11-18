#include "crypto.h"

#include "paths.h"

#include <QFile>
#include <QSaveFile>
#include <QRandomGenerator>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace {

constexpr int kKeyLen = 32;
constexpr int kIvLen = 12;
constexpr int kTagLen = 16;
const char kPrefix[] = "v1:";

QByteArray loadOrCreateKey()
{
    static QByteArray key;
    if (key.size() == kKeyLen)
        return key;

    QFile in(paths::keyFile());
    if (in.open(QIODevice::ReadOnly)) {
        const QByteArray stored = QByteArray::fromBase64(in.readAll().trimmed());
        if (stored.size() == kKeyLen) {
            key = stored;
            return key;
        }
    }

    key = crypto::randomBytes(kKeyLen);
    QSaveFile out(paths::keyFile());
    if (out.open(QIODevice::WriteOnly)) {
        out.write(key.toBase64());
        out.commit();
        QFile::setPermissions(paths::keyFile(),
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    return key;
}

} // namespace

namespace crypto {

QByteArray randomBytes(int count)
{
    QByteArray bytes(count, '\0');
    if (count <= 0)
        return bytes;
    if (RAND_bytes(reinterpret_cast<unsigned char *>(bytes.data()), count) != 1) {
        auto *generator = QRandomGenerator::system();
        for (int i = 0; i < count; ++i)
            bytes[i] = static_cast<char>(generator->bounded(256));
    }
    return bytes;
}

QString encryptString(const QString &plain)
{
    if (plain.isEmpty())
        return QString();

    const QByteArray key = loadOrCreateKey();
    const QByteArray iv = randomBytes(kIvLen);
    const QByteArray input = plain.toUtf8();

    QByteArray cipher(input.size() + 32, '\0');
    QByteArray tag(kTagLen, '\0');
    bool ok = false;
    int total = 0;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (ctx
        && EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kIvLen, nullptr) == 1
        && EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                              reinterpret_cast<const unsigned char *>(key.constData()),
                              reinterpret_cast<const unsigned char *>(iv.constData())) == 1) {
        int len = 0;
        if (EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char *>(cipher.data()), &len,
                              reinterpret_cast<const unsigned char *>(input.constData()),
                              input.size()) == 1) {
            total = len;
            if (EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(cipher.data()) + total,
                                    &len) == 1) {
                total += len;
                ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagLen, tag.data()) == 1;
            }
        }
    }
    if (ctx)
        EVP_CIPHER_CTX_free(ctx);

    if (!ok)
        return QString();

    cipher.truncate(total);
    const QByteArray blob = iv + tag + cipher;
    return QString::fromLatin1(kPrefix) + QString::fromLatin1(blob.toBase64());
}

QString decryptString(const QString &encrypted)
{
    if (encrypted.isEmpty() || !encrypted.startsWith(QLatin1String(kPrefix)))
        return QString();

    const QByteArray blob =
        QByteArray::fromBase64(encrypted.mid(int(sizeof(kPrefix)) - 1).toLatin1());
    if (blob.size() < kIvLen + kTagLen)
        return QString();

    const QByteArray key = loadOrCreateKey();
    const QByteArray iv = blob.left(kIvLen);
    const QByteArray tag = blob.mid(kIvLen, kTagLen);
    const QByteArray cipher = blob.mid(kIvLen + kTagLen);

    QByteArray plain(cipher.size() + 32, '\0');
    QByteArray tagCopy = tag;
    bool ok = false;
    int total = 0;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (ctx
        && EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kIvLen, nullptr) == 1
        && EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                              reinterpret_cast<const unsigned char *>(key.constData()),
                              reinterpret_cast<const unsigned char *>(iv.constData())) == 1) {
        int len = 0;
        if (EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char *>(plain.data()), &len,
                              reinterpret_cast<const unsigned char *>(cipher.constData()),
                              cipher.size()) == 1) {
            total = len;
            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagLen, tagCopy.data()) == 1
                && EVP_DecryptFinal_ex(ctx,
                                       reinterpret_cast<unsigned char *>(plain.data()) + total,
                                       &len) == 1) {
                total += len;
                ok = true;
            }
        }
    }
    if (ctx)
        EVP_CIPHER_CTX_free(ctx);

    if (!ok)
        return QString();

    plain.truncate(total);
    return QString::fromUtf8(plain);
}

void cleanse(QByteArray &buffer)
{
    if (buffer.isEmpty())
        return;
    OPENSSL_cleanse(buffer.data(), size_t(buffer.size()));
    buffer.clear();
}

bool selfTest()
{
    const QString secret = QStringLiteral("p@ssw0rd con spazi e simboli \u00e8\u00e8");
    const QString encrypted = encryptString(secret);
    if (encrypted.isEmpty() || encrypted == secret)
        return false;
    if (decryptString(encrypted) != secret)
        return false;

    // Tampering with the payload must fail authentication.
    QString broken = encrypted;
    const int pos = broken.size() - 3;
    broken[pos] = (broken[pos] == QLatin1Char('A')) ? QLatin1Char('B') : QLatin1Char('A');
    if (!decryptString(broken).isEmpty())
        return false;

    if (decryptString(QStringLiteral("garbage")) != QString())
        return false;
    if (encryptString(QString()) != QString())
        return false;
    return true;
}

} // namespace crypto
