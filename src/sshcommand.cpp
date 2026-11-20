#include "sshcommand.h"

#include "crypto.h"
#include "paths.h"
#include "snippets.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUuid>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

#ifdef _WIN32
// OpenSSH for Windows does not run ProxyCommand through a shell: the expanded
// string is handed to CreateProcessW as a command line, so arguments follow
// the CommandLineToArgvW rules (double quotes, backslash escaping) and single
// quotes are not special.
QString shellQuote(const QString &value)
{
    if (!value.isEmpty()
        && !value.contains(QRegularExpression(QStringLiteral("[\\s\"]"))))
        return value;

    QString quoted = QStringLiteral("\"");
    int backslashes = 0;
    for (const QChar c : value) {
        if (c == QLatin1Char('\\')) {
            ++backslashes;
            continue;
        }
        if (c == QLatin1Char('"'))
            quoted += QString(backslashes * 2 + 1, QLatin1Char('\\'));
        else
            quoted += QString(backslashes, QLatin1Char('\\'));
        quoted += c;
        backslashes = 0;
    }
    quoted += QString(backslashes * 2, QLatin1Char('\\'));
    quoted += QLatin1Char('"');
    return quoted;
}

// Path values inside "-o Option=value": OpenSSH splits some of them (e.g.
// UserKnownHostsFile accepts a list) on whitespace, and Windows profile paths
// may contain spaces ("C:/Users/Mario Rossi/...").
QString optionPath(const QString &path)
{
    return path.contains(QLatin1Char(' ')) ? QLatin1Char('"') + path + QLatin1Char('"') : path;
}

QString sshExecutable()
{
    // OpenSSH client shipped with Windows 10/11 (optional feature, on by default).
    QString exe = QStandardPaths::findExecutable(QStringLiteral("ssh"));
    if (exe.isEmpty()) {
        const QString builtin = qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows"))
                                + QStringLiteral("\\System32\\OpenSSH\\ssh.exe");
        if (QFileInfo::exists(builtin))
            exe = builtin;
    }
    return exe.isEmpty() ? QStringLiteral("ssh.exe") : QDir::toNativeSeparators(exe);
}
#else
QString shellQuote(const QString &value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

QString optionPath(const QString &path)
{
    return path;
}

QString sshExecutable()
{
    const QString exe = QStandardPaths::findExecutable(QStringLiteral("ssh"));
    return exe.isEmpty() ? QStringLiteral("/usr/bin/ssh") : exe;
}
#endif

bool safeHostToken(const QString &value)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9._:\\[\\]%-]+$"));
    return !value.isEmpty() && re.match(value).hasMatch();
}

bool parsePort(const QString &text, quint16 *port)
{
    bool ok = false;
    const uint value = text.toUInt(&ok);
    if (!ok || value == 0 || value > 65535)
        return false;
    *port = static_cast<quint16>(value);
    return true;
}

QStringList authOptions(const Connection &connection)
{
    QStringList options;
    switch (connection.auth) {
    case AuthType::KeyFile:
        options << QStringLiteral("-o") << QStringLiteral("PreferredAuthentications=publickey")
                << QStringLiteral("-o") << QStringLiteral("IdentitiesOnly=yes");
        if (!connection.keyPath.trimmed().isEmpty())
            options << QStringLiteral("-i") << expandUserPath(connection.keyPath);
        break;
    case AuthType::Password:
        options << QStringLiteral("-o")
                << QStringLiteral("PreferredAuthentications=password,keyboard-interactive");
        break;
    case AuthType::Agent:
        options << QStringLiteral("-o") << QStringLiteral("PreferredAuthentications=publickey");
        break;
    }
    return options;
}

QStringList commonOptions()
{
    return {QStringLiteral("-o"),
            QStringLiteral("UserKnownHostsFile=") + optionPath(paths::knownHostsFile()),
            // Unknown hosts are confirmed explicitly: the askpass helper shows
            // the key fingerprint (see confirmHostKey()).
            QStringLiteral("-o"), QStringLiteral("StrictHostKeyChecking=ask"),
            QStringLiteral("-o"), QStringLiteral("ConnectTimeout=20"),
            QStringLiteral("-o"), QStringLiteral("NumberOfPasswordPrompts=1"),
            QStringLiteral("-o"), QStringLiteral("ServerAliveInterval=30"),
            QStringLiteral("-o"), QStringLiteral("ServerAliveCountMax=3")};
}

QString hopCommand(const QString &ssh, const Connection &hop, const QString &nestedProxy)
{
    QStringList args = commonOptions();
    args << authOptions(hop);
    if (!nestedProxy.isEmpty())
        args << QStringLiteral("-o") << QStringLiteral("ProxyCommand=") + nestedProxy;
    if (hop.port != 22)
        args << QStringLiteral("-p") << QString::number(hop.port);
    args << QStringLiteral("-W") << QStringLiteral("%h:%p");
    args << hop.user + QLatin1Char('@') + hop.host;

    QStringList quoted;
    quoted.reserve(args.size());
    for (const QString &arg : args)
        quoted << shellQuote(arg);
#ifdef _WIN32
    // the ssh path itself may contain spaces (e.g. C:\Program Files\OpenSSH)
    return shellQuote(ssh) + QLatin1Char(' ') + quoted.join(QLatin1Char(' '));
#else
    return ssh + QLatin1Char(' ') + quoted.join(QLatin1Char(' '));
#endif
}

QString bracketHost(const QString &host)
{
    return host.contains(QLatin1Char(':')) ? QLatin1Char('[') + host + QLatin1Char(']') : host;
}

// "[bind:]port" with an optional bind address.
QString bindSpec(const QString &address, quint16 port)
{
    const QString trimmed = address.trimmed();
    return (trimmed.isEmpty() ? QString() : bracketHost(trimmed) + QLatin1Char(':'))
           + QString::number(port);
}

// Appends the -L/-R/-D option for one tunnel, or explains what is missing.
bool appendTunnel(const TunnelConfig &tunnel, const QString &connectionName, QStringList *args,
                  QString *error)
{
    const QString remoteHost = tunnel.remoteHost.trimmed();
    QString localHost = tunnel.localAddress.trimmed();

    switch (tunnel.type) {
    case TunnelType::Local:
        if (tunnel.localPort == 0 || tunnel.remotePort == 0 || remoteHost.isEmpty())
            break;
        *args << QStringLiteral("-L")
              << bindSpec(localHost, tunnel.localPort) + QLatin1Char(':') + bracketHost(remoteHost)
                     + QLatin1Char(':') + QString::number(tunnel.remotePort);
        return true;
    case TunnelType::Remote:
        // Listen on the server, connect to localAddress:localPort from here.
        if (tunnel.localPort == 0 || tunnel.remotePort == 0)
            break;
        if (localHost.isEmpty())
            localHost = QStringLiteral("localhost");
        *args << QStringLiteral("-R")
              << bindSpec(remoteHost, tunnel.remotePort) + QLatin1Char(':') + bracketHost(localHost)
                     + QLatin1Char(':') + QString::number(tunnel.localPort);
        return true;
    case TunnelType::Dynamic:
        if (tunnel.localPort == 0)
            break;
        *args << QStringLiteral("-D") << bindSpec(localHost, tunnel.localPort);
        return true;
    }
    *error = QStringLiteral("Tunnel incompleto: controlla porte e host di \"%1\".").arg(connectionName);
    return false;
}

bool writePrivateFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(contents);
    file.close();
    return QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

// Embedded keys never touch the configuration directory: they are written to a
// 0600 file inside the 0700 runtime directory and removed with the session.
bool materializeKey(Connection &connection, const QString &runtimeDir, QStringList *files,
                    QString *error)
{
    if (connection.auth != AuthType::KeyFile || connection.privateKey.isEmpty())
        return true;

    const QString path = QDir(runtimeDir).filePath(
        QStringLiteral("key-") + QUuid::createUuid().toString(QUuid::WithoutBraces)
        + QStringLiteral(".tmp"));
    if (!writePrivateFile(path, connection.privateKey)) {
        *error = QStringLiteral("Impossibile creare il file temporaneo per la chiave \"%1\".")
                     .arg(connection.keyName);
        return false;
    }
    connection.keyPath = path;
    files->append(path);
    return true;
}

QJsonObject askpassEntry(const Connection &connection)
{
    QJsonObject entry;
    entry[QStringLiteral("host")] = connection.host;
    entry[QStringLiteral("password")] = QString::fromUtf8(connection.password);
    entry[QStringLiteral("passphrase")] = QString::fromUtf8(connection.passphrase);
    return entry;
}

QJsonArray askpassEntries(const Connection &connection, const QVector<Connection> &jumpChain)
{
    QJsonArray entries;
    entries.append(askpassEntry(connection));
    for (const Connection &hop : jumpChain)
        entries.append(askpassEntry(hop));
    return entries;
}

bool validTargets(const Connection &connection, const QVector<Connection> &jumpChain,
                  QString *error)
{
    if (!safeHostToken(connection.host)) {
        *error = QStringLiteral("Host non valido: usa solo lettere, numeri, '.', '-', ':' o '[]'.");
        return false;
    }
    if (!safeHostToken(connection.user)) {
        *error = QStringLiteral("Utente non valido: usa solo lettere, numeri, '.', '_' o '-'.");
        return false;
    }
    for (const Connection &hop : jumpChain) {
        if (!safeHostToken(hop.host) || !safeHostToken(hop.user)) {
            *error = QStringLiteral("Jump host non valido: controlla host e utente di \"%1\".")
                         .arg(hop.displayName());
            return false;
        }
    }
    return true;
}

// Materializes embedded keys and the askpass document: terminal sessions and
// file transfers authenticate exactly the same way.
bool preparePrivateFiles(Connection &effective, QVector<Connection> &hops,
                         const QString &runtimeDir, QStringList *files, QString *askpassPath,
                         QString *error)
{
    for (Connection &hop : hops) {
        if (!materializeKey(hop, runtimeDir, files, error))
            return false;
    }
    if (!materializeKey(effective, runtimeDir, files, error))
        return false;

    QJsonObject askpassConfig;
    askpassConfig[QStringLiteral("entries")] = askpassEntries(effective, hops);

    QByteArray askpassContents = QJsonDocument(askpassConfig).toJson(QJsonDocument::Compact);
    const QString path = QDir(runtimeDir).filePath(
        QStringLiteral("askpass-") + QUuid::createUuid().toString(QUuid::WithoutBraces)
        + QStringLiteral(".json"));
    const bool written = writePrivateFile(path, askpassContents);
    crypto::cleanse(askpassContents);
    if (!written) {
        *error = QStringLiteral("Impossibile creare il file temporaneo delle credenziali in %1.")
                     .arg(runtimeDir);
        return false;
    }
    *askpassPath = path;
    files->append(path);
    return true;
}

QString proxyOption(const QString &ssh, const QVector<Connection> &hops)
{
    QString proxy;
    for (int i = hops.size() - 1; i >= 0; --i)
        proxy = hopCommand(ssh, hops.at(i), proxy);
    return proxy;
}

QStringList askpassEnvironment(const QString &askpassFile)
{
    QStringList environment;
#ifdef _WIN32
    // The askpass helper may show the host key dialog: let it come to the
    // foreground instead of only flashing in the taskbar. Called right after
    // a user action, while this process owns the foreground.
    AllowSetForegroundWindow(ASFW_ANY);
    // OpenSSH for Windows honours SSH_ASKPASS(_REQUIRE) and needs no DISPLAY.
    environment << QStringLiteral("SSH_ASKPASS=")
                       + QDir::toNativeSeparators(QCoreApplication::applicationFilePath())
                << QStringLiteral("SSH_ASKPASS_REQUIRE=force")
                << QStringLiteral("SSHMANAGER_ASKPASS_FILE=") + QDir::toNativeSeparators(askpassFile);
#else
    environment << QStringLiteral("SSH_ASKPASS=") + QCoreApplication::applicationFilePath()
                << QStringLiteral("SSH_ASKPASS_REQUIRE=force")
                << QStringLiteral("SSHMANAGER_ASKPASS_FILE=") + askpassFile;
    if (qEnvironmentVariableIsEmpty("DISPLAY"))
        environment << QStringLiteral("DISPLAY=:0");
#endif
    return environment;
}

} // namespace

QString expandUserPath(const QString &path)
{
    const QString trimmed = path.trimmed();
    if (trimmed == QLatin1String("~"))
        return QDir::homePath();
    if (trimmed.startsWith(QLatin1String("~/")))
        return QDir::homePath() + trimmed.mid(1);
#ifdef _WIN32
    if (trimmed.startsWith(QLatin1String("~\\")))
        return QDir::homePath() + QLatin1Char('/') + trimmed.mid(2);
#endif
    return trimmed;
}

bool parseQuickTarget(const QString &text, QString *user, QString *host, quint16 *port,
                      QString *normalized)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || trimmed.contains(QRegularExpression(QStringLiteral("\\s"))))
        return false;

    QString userPart = qEnvironmentVariable("USER", qEnvironmentVariable("USERNAME"));
    if (userPart.isEmpty())
        userPart = QStringLiteral("root");

    QString rest = trimmed;
    bool explicitUser = false;
    const int at = rest.lastIndexOf(QLatin1Char('@'));
    if (at >= 0) {
        explicitUser = true;
        userPart = rest.left(at);
        rest = rest.mid(at + 1);
    }
    if (rest.isEmpty() || userPart.isEmpty())
        return false;

    QString hostPart = rest;
    quint16 parsedPort = 22;
    if (rest.startsWith(QLatin1Char('['))) {
        const int close = rest.indexOf(QLatin1Char(']'));
        if (close < 0)
            return false;
        hostPart = rest.mid(1, close - 1);
        const QString tail = rest.mid(close + 1);
        if (!tail.isEmpty()
            && (!tail.startsWith(QLatin1Char(':')) || !parsePort(tail.mid(1), &parsedPort)))
            return false;
    } else {
        const int firstColon = rest.indexOf(QLatin1Char(':'));
        const int lastColon = rest.lastIndexOf(QLatin1Char(':'));
        // Exactly one colon is host:port; several colons are a bare IPv6 host.
        if (firstColon >= 0 && firstColon == lastColon) {
            if (!parsePort(rest.mid(firstColon + 1), &parsedPort))
                return false;
            hostPart = rest.left(firstColon);
        }
    }

    if (hostPart.isEmpty() || !safeHostToken(userPart) || !safeHostToken(hostPart))
        return false;
    if (!explicitUser && !hostPart.contains(QLatin1Char('.'))
        && !hostPart.contains(QLatin1Char(':')) && hostPart != QLatin1String("localhost"))
        return false;

    if (user)
        *user = userPart;
    if (host)
        *host = hostPart;
    if (port)
        *port = parsedPort;
    if (normalized) {
        const QString shownHost = hostPart.contains(QLatin1Char(':'))
                                      ? QLatin1Char('[') + hostPart + QLatin1Char(']')
                                      : hostPart;
        QString value = userPart + QLatin1Char('@') + shownHost;
        if (parsedPort != 22)
            value += QLatin1Char(':') + QString::number(parsedPort);
        *normalized = value;
    }
    return true;
}

SshPlan buildQuickSshPlan(const QString &user, const QString &host, quint16 port)
{
    SshPlan plan;
    if (!safeHostToken(user) || !safeHostToken(host)) {
        plan.error = QStringLiteral("Destinazione non valida: controlla utente e host.");
        return plan;
    }
    if (port == 0) {
        plan.error = QStringLiteral("Porta non valida: usa un valore tra 1 e 65535.");
        return plan;
    }

    plan.program = sshExecutable();
    // No askpass file here and no PreferredAuthentications restriction: ssh can
    // use the agent, a key, or ask for a password directly in the terminal.
    plan.args << QStringLiteral("-o")
              << QStringLiteral("UserKnownHostsFile=") + optionPath(paths::knownHostsFile())
              // No askpass here: ssh asks to confirm a new host key in the terminal.
              << QStringLiteral("-o") << QStringLiteral("StrictHostKeyChecking=ask")
              << QStringLiteral("-o") << QStringLiteral("ConnectTimeout=20")
              << QStringLiteral("-o") << QStringLiteral("ServerAliveInterval=30")
              << QStringLiteral("-o") << QStringLiteral("ServerAliveCountMax=3")
              << QStringLiteral("-p") << QString::number(port)
              << user + QLatin1Char('@') + host;
    plan.environment << QStringLiteral("TERM=xterm-256color");
    plan.valid = true;
    return plan;
}

enum class PlanMode { Session, Tunnels, Command };

static SshPlan buildPlan(const Connection &connection, const QVector<Connection> &jumpChain,
                         const QString &runtimeDir, PlanMode mode)
{
    SshPlan plan;
    const QString ssh = sshExecutable();
    const bool tunnelsOnly = mode == PlanMode::Tunnels;

    if (!validTargets(connection, jumpChain, &plan.error))
        return plan;

    // Validate the tunnels before any credential touches the disk. A remote
    // command runs without them: it must not fight a session over the ports.
    QStringList tunnelArgs;
    for (const TunnelConfig &tunnel : connection.tunnels) {
        if (mode != PlanMode::Command && tunnel.enabled
            && !appendTunnel(tunnel, connection.displayName(), &tunnelArgs, &plan.error))
            return plan;
    }
    if (tunnelsOnly && tunnelArgs.isEmpty()) {
        plan.error = QStringLiteral("\"%1\" non ha tunnel attivi da avviare.")
                         .arg(connection.displayName());
        return plan;
    }

    Connection effective = connection;
    QVector<Connection> hops = jumpChain;

    QString askpassFile;
    if (!preparePrivateFiles(effective, hops, runtimeDir, &plan.temporaryFiles, &askpassFile,
                             &plan.error))
        return plan;

    plan.program = ssh;
    plan.args = commonOptions();
    plan.args << authOptions(effective);
    if (tunnelsOnly) {
        // No remote command and no terminal; exit (and get restarted) when a
        // forward cannot be set up instead of running half-broken.
        plan.args << QStringLiteral("-N") << QStringLiteral("-T")
                  << QStringLiteral("-o") << QStringLiteral("ExitOnForwardFailure=yes");
    } else if (mode == PlanMode::Command) {
        // No terminal: plain stdout/stderr and the remote exit status.
        plan.args << QStringLiteral("-T");
    }
    plan.args << tunnelArgs;

    if (!hops.isEmpty())
        plan.args << QStringLiteral("-o") << QStringLiteral("ProxyCommand=") + proxyOption(ssh, hops);

    // Startup command: run it, then hand over to the user's login shell, so
    // the session stays interactive even if the command fails or exits.
    const QString startup = mode != PlanMode::Session
                                ? QString()
                                : snippets::expandBuiltins(effective.startupCommand.trimmed(),
                                                           effective);
    if (!startup.isEmpty())
        plan.args << QStringLiteral("-t");

    plan.args << QStringLiteral("-p") << QString::number(effective.port);
    plan.args << effective.user + QLatin1Char('@') + effective.host;
    if (!startup.isEmpty())
        plan.args << startup + QStringLiteral("; exec \"${SHELL:-/bin/sh}\" -l");

    plan.askpassFile = askpassFile;
    if (mode == PlanMode::Session)
        plan.environment << QStringLiteral("TERM=xterm-256color");
    plan.environment << askpassEnvironment(askpassFile);

    plan.valid = true;
    return plan;
}

SshPlan buildSshPlan(const Connection &connection, const QVector<Connection> &jumpChain,
                     const QString &runtimeDir)
{
    return buildPlan(connection, jumpChain, runtimeDir, PlanMode::Session);
}

SshPlan buildTunnelPlan(const Connection &connection, const QVector<Connection> &jumpChain,
                        const QString &runtimeDir)
{
    return buildPlan(connection, jumpChain, runtimeDir, PlanMode::Tunnels);
}

SshPlan buildCommandPlan(const Connection &connection, const QVector<Connection> &jumpChain,
                         const QString &runtimeDir)
{
    return buildPlan(connection, jumpChain, runtimeDir, PlanMode::Command);
}

SftpPlan buildSftpPlan(const Connection &connection, const QVector<Connection> &jumpChain,
                       const QString &runtimeDir)
{
    SftpPlan plan;
    const QString ssh = sshExecutable();

    if (!validTargets(connection, jumpChain, &plan.error))
        return plan;

    Connection effective = connection;
    QVector<Connection> hops = jumpChain;

    QString askpassFile;
    if (!preparePrivateFiles(effective, hops, runtimeDir, &plan.temporaryFiles, &askpassFile,
                             &plan.error))
        return plan;

    // No tunnels here: the transfer only needs the SFTP subsystem.
    plan.program = ssh;
    plan.args = commonOptions();
    plan.args << authOptions(effective);
    if (!hops.isEmpty())
        plan.args << QStringLiteral("-o") << QStringLiteral("ProxyCommand=") + proxyOption(ssh, hops);
    plan.args << QStringLiteral("-p") << QString::number(effective.port);
    plan.args << QStringLiteral("-s");
    plan.args << effective.user + QLatin1Char('@') + effective.host;
    plan.args << QStringLiteral("sftp");

    plan.environment = askpassEnvironment(askpassFile);
    plan.valid = true;
    return plan;
}
