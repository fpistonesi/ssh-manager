#pragma once

#include "model.h"
#include "sshcommand.h"

#include <QDateTime>
#include <QObject>
#include <QStringList>

class QProcess;
class QTimer;

// Tunnels of one connection run as a background "ssh -N" process, without a
// terminal, restarted automatically when the connection drops.
//
// The plan's temporary files (embedded key, askpass document) are kept for
// the whole life of the service, so it can reconnect while the vault is
// locked; they are deleted when the service is destroyed.
class TunnelService : public QObject {
    Q_OBJECT

public:
    enum class State {
        Starting, // ssh launched, authentication / forwards not confirmed yet
        Running,  // authenticated and every forward set up
        Retrying, // dropped: waiting before the next attempt
        Failed,   // gave up (authentication, host key, missing ssh)
        Stopped,
    };

    // `connection` must already be sanitized: only names and tunnels are used.
    TunnelService(const Connection &connection, const SshPlan &plan, QObject *parent = nullptr);
    ~TunnelService() override;

    const Connection &connection() const { return connection_; }
    State state() const { return state_; }
    QString lastError() const { return lastError_; }
    int secondsToRetry() const;
    qint64 processId() const;
    QDateTime runningSince() const { return runningSince_; }
    // Last lines of ssh's output (debug included), for diagnostics.
    QString log() const { return log_.join(QLatin1Char('\n')); }

    void start();
    void stop();
    void restart();

signals:
    void stateChanged();

private:
    void launch();
    void readOutput();
    void handleFinished();
    void setState(State state);
    void checkReady();
    void markRunning();
    QString errorSummary() const;
    bool fatalError() const;

    Connection connection_;
    SshPlan plan_;
    QProcess *process_ = nullptr;
    QTimer *retryTimer_ = nullptr;
    QTimer *readyTimer_ = nullptr;
    QList<quint16> localPorts_; // -L / -D listeners that must appear
    QDateTime authenticatedAt_;
    State state_ = State::Stopped;
    QString lastError_;
    QStringList log_;
    QByteArray pending_;
    QDateTime runningSince_;
    QDateTime retryAt_;
    int attempt_ = 0;
    int remoteForwards_ = 0;
    int remoteConfirmed_ = 0;
    bool interactive_ = false;
    bool stopping_ = false;
};
