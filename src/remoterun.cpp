#include "remoterun.h"

#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>

namespace {

constexpr qsizetype kOutputLimit = 1024 * 1024; // per stream

} // namespace

RemoteRun::RemoteRun(const SshPlan &plan, const QString &remoteCommand, const QByteArray &input,
                     int timeoutMs, QObject *parent)
    : QObject(parent), plan_(plan), command_(remoteCommand), input_(input), timeoutMs_(timeoutMs)
{
}

RemoteRun::~RemoteRun()
{
    if (process_) {
        process_->disconnect(this);
        if (process_->state() != QProcess::NotRunning) {
            process_->kill();
            process_->waitForFinished(2000);
        }
    }
}

bool RemoteRun::isRunning() const
{
    return process_ && !done_;
}

void RemoteRun::start()
{
    clock_.start();
    if (!QFileInfo(plan_.program).isExecutable()) {
        result_.startError = tr("Il client \"ssh\" non è stato trovato in %1.").arg(plan_.program);
        complete();
        return;
    }

    process_ = new QProcess(this);
    process_->setProgram(plan_.program);
    process_->setArguments(plan_.args + QStringList{command_});
    process_->setProcessChannelMode(QProcess::SeparateChannels);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const QString &entry : plan_.environment) {
        const int equals = entry.indexOf(QLatin1Char('='));
        if (equals > 0)
            environment.insert(entry.left(equals), entry.mid(equals + 1));
    }
    process_->setProcessEnvironment(environment);

    connect(process_, &QProcess::readyReadStandardOutput, this,
            [this] { append(result_.out, process_->readAllStandardOutput()); });
    connect(process_, &QProcess::readyReadStandardError, this,
            [this] { append(result_.err, process_->readAllStandardError()); });
    connect(process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        append(result_.out, process_->readAllStandardOutput());
        append(result_.err, process_->readAllStandardError());
        if (!result_.timedOut && !result_.cancelled)
            result_.exitCode = status == QProcess::NormalExit ? code : -1;
        complete();
    });
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            result_.startError = tr("Impossibile avviare %1.").arg(plan_.program);
            complete();
        }
    });

    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, [this] {
        result_.timedOut = true;
        process_->kill();
    });
    if (timeoutMs_ > 0)
        timer_->start(timeoutMs_);

    process_->start();
    // The script (if any) goes to the remote standard input; closing the
    // channel tells the remote side there is nothing more.
    if (!input_.isEmpty())
        process_->write(input_);
    process_->closeWriteChannel();
}

void RemoteRun::cancel()
{
    if (!process_ || done_)
        return;
    result_.cancelled = true;
    process_->kill();
}

void RemoteRun::append(QByteArray &target, const QByteArray &data)
{
    if (data.isEmpty())
        return;
    const qsizetype room = kOutputLimit - target.size();
    if (room <= 0) {
        result_.truncated = true;
        return;
    }
    if (data.size() > room)
        result_.truncated = true;
    target.append(data.left(room));
}

void RemoteRun::complete()
{
    if (done_)
        return;
    done_ = true;
    if (timer_)
        timer_->stop();
    result_.elapsedMs = clock_.elapsed();
    emit finished(result_);
}
