#pragma once

#include "model.h"

#include <QString>
#include <QStringList>
#include <QVector>

// A fully resolved ssh invocation: program, argv and environment for the
// terminal's child process, plus temporary files to delete afterwards.
struct SshPlan {
    bool valid = false;
    QString error;
    QString program;
    QStringList args;
    QStringList environment;
    QString askpassFile;
    QStringList temporaryFiles;
};

// A resolved "ssh -s <host> sftp" invocation for file transfers. It uses the
// same options, credentials and temporary files as a terminal session.
struct SftpPlan {
    bool valid = false;
    QString error;
    QString program;
    QStringList args;
    QStringList environment;
    QStringList temporaryFiles;
};

// jumpChain[0] is the jump host closest to the destination, followed by the
// jump host of that jump host, and so on.
SshPlan buildSshPlan(const Connection &connection, const QVector<Connection> &jumpChain,
                     const QString &runtimeDir);
// Background tunnels only: "ssh -N" with ExitOnForwardFailure, no terminal.
// Fails when the connection has no enabled tunnel.
SshPlan buildTunnelPlan(const Connection &connection, const QVector<Connection> &jumpChain,
                        const QString &runtimeDir);
// Non-interactive command execution ("ssh -T host <command>"): no terminal,
// no tunnels, no startup command. The remote command is appended by the
// caller (see RemoteRun).
SshPlan buildCommandPlan(const Connection &connection, const QVector<Connection> &jumpChain,
                         const QString &runtimeDir);
SftpPlan buildSftpPlan(const Connection &connection, const QVector<Connection> &jumpChain,
                       const QString &runtimeDir);

QString expandUserPath(const QString &path);

// Ad-hoc destination for the quick-connect palette. No credentials are stored:
// ssh prompts in the terminal (or uses the agent).
SshPlan buildQuickSshPlan(const QString &user, const QString &host, quint16 port);

// Parses "host", "user@host", "host:port" or "user@host:port". A bare word is
// only accepted as host when an explicit user is present, so palette filtering
// does not turn every query into a connection attempt.
bool parseQuickTarget(const QString &text, QString *user, QString *host, quint16 *port,
                      QString *normalized = nullptr);
