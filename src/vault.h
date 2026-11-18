#pragma once

#include <QByteArray>
#include <QString>

// Encrypted container for connections and private keys.
//
// File layout:
//   magic "SSHManagerVault1" | u32 header length | header JSON
//   | u32 nonce length | nonce | 16-byte GCM tag | ciphertext
//
// The header (salt + KDF iterations) is authenticated as AAD. The AES key is
// derived from the master password with PBKDF2-HMAC-SHA256 and only kept in
// memory while the vault is unlocked; lock() wipes it.
class Vault {
public:
    explicit Vault(QString path = QString());
    ~Vault();

    QString path() const { return m_path; }
    bool exists() const;
    bool isUnlocked() const { return m_unlocked; }

    bool create(const QByteArray &password, const QByteArray &payload, QString *error);
    bool unlock(const QByteArray &password, QByteArray *payload, QString *error);
    bool save(const QByteArray &payload, QString *error);
    bool changePassword(const QByteArray &currentPassword, const QByteArray &newPassword,
                        const QByteArray &payload, QString *error);

    // Writes an encrypted archive to an arbitrary destination, protected by a
    // dedicated password (used for exports). Independent of the current key.
    static bool writeArchive(const QString &destination, const QByteArray &password,
                             const QByteArray &payload, QString *error);
    void lock();

private:
    bool writePayload(const QByteArray &payload, QString *error);
    bool readFile(QByteArray *header, QByteArray *nonce, QByteArray *tag, QByteArray *cipher,
                  QString *error) const;

    QString m_path;
    QByteArray m_key;
    QByteArray m_salt;
    quint32 m_iterations = 600000;
    bool m_unlocked = false;
};
