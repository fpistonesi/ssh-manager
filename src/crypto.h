#pragma once

#include <QByteArray>
#include <QString>

namespace crypto {

QByteArray randomBytes(int count);

// Legacy (pre-vault) helpers, still needed to migrate old stores.
QString encryptString(const QString &plain);
QString decryptString(const QString &encrypted);

// Overwrites the buffer with zeros and empties it.
void cleanse(QByteArray &buffer);

bool selfTest();

} // namespace crypto
