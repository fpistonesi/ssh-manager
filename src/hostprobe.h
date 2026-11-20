#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>

// Whether an SSH server answers: a TCP connection, the server's
// identification line ("SSH-2.0-OpenSSH_9.6"), then an immediate close. No key
// exchange and no login attempt are made, but the server still logs the
// connection.
class HostProbe : public QObject {
    Q_OBJECT

public:
    struct Result {
        enum class State { Unknown, Reachable, Unreachable };
        State state = State::Unknown;
        int latencyMs = -1;
        QString banner; // without the "SSH-2.0-" prefix
        QString error;
        QDateTime checkedAt;
    };

    explicit HostProbe(QObject *parent = nullptr);

    // Starts a check; result() arrives later with the same key. A check
    // already running for the key is not duplicated.
    void check(const QString &key, const QString &host, quint16 port, int timeoutMs = 5000);
    bool isChecking(const QString &key) const;

signals:
    void result(const QString &key, const HostProbe::Result &result);

private:
    QStringList running_;
};
