#include "paths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace paths {

namespace {

// ~/.config/sshmanager (Linux), %LOCALAPPDATA%\sshmanager (Windows).
QString localConfigDir()
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

} // namespace

QString portableDir()
{
    static const QString cached = [] {
        if (!QCoreApplication::instance())
            return QString();
        const QString dir = QCoreApplication::applicationDirPath();
        return QFileInfo(dir + QStringLiteral("/vault.db")).isFile() ? dir : QString();
    }();
    return cached;
}

bool isPortable()
{
    return !portableDir().isEmpty();
}

QString configDir()
{
    // The executable's folder keeps its own permissions.
    return isPortable() ? portableDir() : localConfigDir();
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

    cached = localConfigDir() + QStringLiteral("/run");
    QDir().mkpath(cached);
    QFile::setPermissions(cached,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return cached;
}

} // namespace paths
