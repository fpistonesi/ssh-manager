#pragma once

#include "sshcommand.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QStringList>

class QProcess;
class QTimer;

// One non-interactive "ssh -T host <command>" run (plan from
// buildCommandPlan()), collecting stdout, stderr and the exit status. The
// plan's temporary files belong to the caller.
class RemoteRun : public QObject {
    Q_OBJECT

public:
    struct Result {
        int exitCode = -1;
        QByteArray out;
        QByteArray err;
        bool timedOut = false;
        bool cancelled = false;
        bool truncated = false;  // output beyond the limit was dropped
        QString startError;      // ssh could not be started
        qint64 elapsedMs = 0;
        bool ok() const { return startError.isEmpty() && !timedOut && !cancelled && exitCode == 0; }
    };

    // `remoteCommand` is passed to the remote shell as is; `input` (if any) is
    // written to its standard input, which is then closed.
    RemoteRun(const SshPlan &plan, const QString &remoteCommand, const QByteArray &input,
              int timeoutMs, QObject *parent = nullptr);
    ~RemoteRun() override;

    void start();
    void cancel();
    bool isRunning() const;

signals:
    void finished(const RemoteRun::Result &result);

private:
    void append(QByteArray &target, const QByteArray &data);
    void complete();

    SshPlan plan_;
    QString command_;
    QByteArray input_;
    int timeoutMs_ = 0;
    QProcess *process_ = nullptr;
    QTimer *timer_ = nullptr;
    QElapsedTimer clock_;
    Result result_;
    bool done_ = false;
};
