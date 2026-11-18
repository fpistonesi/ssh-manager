#include "paths.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>

namespace paths {

QString configDir()
{
    static QString cached;
    if (!cached.isEmpty())
        return cached;

    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.config");

    cached = base + QStringLiteral("/sshmanager");
    QDir().mkpath(cached);
    QFile::setPermissions(cached,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return cached;
}

QString knownHostsFile()
{
    return configDir() + QStringLiteral("/known_hosts");
}

QString storeFile()
{
    return configDir() + QStringLiteral("/connections.json");
}

QString keyFile()
{
    return configDir() + QStringLiteral("/master.key");
}

QString vaultFile()
{
    return configDir() + QStringLiteral("/vault.db");
}

QString runtimeDir()
{
    static QString cached;
    if (!cached.isEmpty())
        return cached;

    cached = configDir() + QStringLiteral("/run");
    QDir().mkpath(cached);
    QFile::setPermissions(cached,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return cached;
}

} // namespace paths
