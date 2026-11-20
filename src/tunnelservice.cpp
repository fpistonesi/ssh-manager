#include "tunnelservice.h"

#include "tunnelprobe.h"

#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>

#include <iterator>

namespace {

// Seconds to wait before attempt n (1-based); the last value repeats.
constexpr int kRetryDelays[] = {5, 15, 30, 60};
// A connection that stayed up this long counts as healthy again.
constexpr int kHealthySeconds = 60;
constexpr int kLogLines = 40;

} // namespace

TunnelService::TunnelService(const Connection &connection, const SshPlan &plan, QObject *parent)
    : QObject(parent), connection_(connection), plan_(plan)
{
    connection_.clearSensitive();
    for (const TunnelConfig &tunnel : connection_.tunnels) {
        if (!tunnel.enabled)
            continue;
        if (tunnel.type == TunnelType::Remote)
            ++remoteForwards_;
        else
            localPorts_.append(tunnel.localPort);
    }

    // Debug output is what tells authentication and forward setup apart:
    // "Entering interactive session." and "remote forward success".
    plan_.args.prepend(QStringLiteral("-v"));

    retryTimer_ = new QTimer(this);
    retryTimer_->setSingleShot(true);
    connect(retryTimer_, &QTimer::timeout, this, &TunnelService::launch);

    // Fallback readiness check while starting, see checkReady().
    readyTimer_ = new QTimer(this);
    readyTimer_->setInterval(1000);
    connect(readyTimer_, &QTimer::timeout, this, &TunnelService::checkReady);
}

TunnelService::~TunnelService()
{
    stopping_ = true;
    if (process_) {
        process_->disconnect(this);
        if (process_->state() != QProcess::NotRunning) {
            process_->kill();
            process_->waitForFinished(3000);
        }
    }
    for (const QString &path : plan_.temporaryFiles)
        QFile::remove(path);
}

int TunnelService::secondsToRetry() const
{
    if (state_ != State::Retrying || !retryAt_.isValid())
        return 0;
    const qint64 ms = QDateTime::currentDateTime().msecsTo(retryAt_);
    return int(qMax<qint64>(0, (ms + 999) / 1000));
}

qint64 TunnelService::processId() const
{
    return process_ && process_->state() == QProcess::Running ? process_->processId() : 0;
}

void TunnelService::start()
{
    if (state_ == State::Starting || state_ == State::Running)
        return;
    attempt_ = 0;
    retryTimer_->stop();
    launch();
}

void TunnelService::stop()
{
    stopping_ = true;
    retryTimer_->stop();
    readyTimer_->stop();
    if (process_ && process_->state() != QProcess::NotRunning) {
        process_->kill();
        process_->waitForFinished(3000);
    }
    stopping_ = false;
    setState(State::Stopped);
}

void TunnelService::restart()
{
    stop();
    lastError_.clear();
    start();
}

void TunnelService::launch()
{
    if (process_) {
        process_->disconnect(this);
        process_->deleteLater();
        process_ = nullptr;
    }
    log_.clear();
    pending_.clear();
    interactive_ = false;
    remoteConfirmed_ = 0;
    runningSince_ = QDateTime();
    authenticatedAt_ = QDateTime();
    ++attempt_;

    if (!QFileInfo(plan_.program).isExecutable()) {
        lastError_ = tr("Il client \"ssh\" non è stato trovato in %1.").arg(plan_.program);
        setState(State::Failed);
        return;
    }

    process_ = new QProcess(this);
    process_->setProgram(plan_.program);
    process_->setArguments(plan_.args);
    // ssh writes its diagnostics to stderr; with -N stdout stays empty.
    process_->setProcessChannelMode(QProcess::MergedChannels);
    process_->setStandardInputFile(QProcess::nullDevice());

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const QString &entry : plan_.environment) {
        const int equals = entry.indexOf(QLatin1Char('='));
        if (equals > 0)
            environment.insert(entry.left(equals), entry.mid(equals + 1));
    }
    process_->setProcessEnvironment(environment);

    connect(process_, &QProcess::readyRead, this, &TunnelService::readOutput);
    connect(process_, &QProcess::finished, this, &TunnelService::handleFinished);
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            lastError_ = tr("Impossibile avviare %1.").arg(plan_.program);
            setState(State::Failed);
        }
    });

    setState(State::Starting);
    process_->start();
    readyTimer_->start();
}

void TunnelService::readOutput()
{
    pending_ += process_->readAll();
    int newline;
    while ((newline = pending_.indexOf('\n')) >= 0) {
        const QString line = QString::fromLocal8Bit(pending_.left(newline)).trimmed();
        pending_.remove(0, newline + 1);
        if (line.isEmpty())
            continue;

        log_.append(line);
        while (log_.size() > kLogLines)
            log_.removeFirst();

        if (line.contains(QLatin1String("Entering interactive session")))
            interactive_ = true;
        else if (line.contains(QLatin1String("remote forward success")))
            ++remoteConfirmed_;
        else if (line.contains(QLatin1String("Authenticated to"))
                 && !authenticatedAt_.isValid())
            authenticatedAt_ = QDateTime::currentDateTime();

        if (state_ == State::Starting && interactive_ && remoteConfirmed_ >= remoteForwards_)
            markRunning();
    }
}

void TunnelService::checkReady()
{
    if (state_ != State::Starting) {
        readyTimer_->stop();
        return;
    }
    const qint64 pid = processId();
    if (pid <= 0)
        return;

    // Not every OpenSSH build prints the same debug messages. With
    // ExitOnForwardFailure a forward that cannot be set up (a refused -R
    // included) makes ssh exit right after authentication, so a client that
    // is still alive a few seconds later, with every local listener open,
    // has its tunnels up.
    bool supported = false;
    const QSet<quint16> listening = tunnelprobe::listeningPorts(pid, &supported);
    bool allListening = supported && !localPorts_.isEmpty();
    for (quint16 port : localPorts_)
        allListening = allListening && listening.contains(port);
    if (supported && !localPorts_.isEmpty() && !allListening)
        return;

    // ssh opens local listeners only after authenticating: when they are all
    // up, that is proof enough even without the "Authenticated to" line.
    if (!authenticatedAt_.isValid()) {
        if (!allListening)
            return;
        authenticatedAt_ = QDateTime::currentDateTime();
    }
    if (authenticatedAt_.msecsTo(QDateTime::currentDateTime()) >= 3000)
        markRunning();
}

void TunnelService::markRunning()
{
    readyTimer_->stop();
    runningSince_ = QDateTime::currentDateTime();
    lastError_.clear();
    setState(State::Running);
}

void TunnelService::handleFinished()
{
    if (stopping_)
        return;
    readyTimer_->stop();
    if (process_)
        pending_ += process_->readAll();
    if (!pending_.isEmpty()) {
        pending_ += '\n';
        readOutput();
    }

    lastError_ = errorSummary();
    if (fatalError()) {
        setState(State::Failed);
        return;
    }

    // A long healthy run resets the backoff: this is a fresh drop.
    if (runningSince_.isValid()
        && runningSince_.secsTo(QDateTime::currentDateTime()) >= kHealthySeconds)
        attempt_ = 0;

    const int count = int(std::size(kRetryDelays));
    const int delay = kRetryDelays[qBound(0, attempt_ - 1, count - 1)];
    retryAt_ = QDateTime::currentDateTime().addSecs(delay);
    retryTimer_->start(delay * 1000);
    setState(State::Retrying);
}

void TunnelService::setState(State state)
{
    if (state_ == state && state != State::Retrying)
        return;
    state_ = state;
    emit stateChanged();
}

QString TunnelService::errorSummary() const
{
    const auto chatter = [](const QString &line) {
        return line.startsWith(QLatin1String("debug"), Qt::CaseInsensitive)
               || line.startsWith(QLatin1String("OpenSSH"), Qt::CaseInsensitive)
               || line.startsWith(QLatin1String("Transferred:"))
               || line.startsWith(QLatin1String("Bytes per second"))
               || line.startsWith(QLatin1String("Authenticated to"))
               || line.startsWith(QLatin1String("Warning: Permanently added"));
    };
    // Lines that name the actual cause beat the generic last words
    // ("Could not request local forwarding.").
    static const char *const causes[] = {
        "Address already in use", "remote port forwarding failed", "Permission denied",
        "Host key verification failed", "Could not resolve", "Connection refused",
        "Connection timed out", "not responding", "Network is unreachable",
    };
    for (int i = log_.size() - 1; i >= 0; --i) {
        const QString &line = log_.at(i);
        if (chatter(line))
            continue;
        for (const char *cause : causes) {
            if (line.contains(QLatin1String(cause), Qt::CaseInsensitive))
                return line;
        }
    }
    // Otherwise the last line that is not debug chatter.
    for (int i = log_.size() - 1; i >= 0; --i) {
        if (!chatter(log_.at(i)))
            return log_.at(i);
    }
    return tr("La connessione si è chiusa");
}

bool TunnelService::fatalError() const
{
    // Retrying these would only lock the account or hide a real problem.
    static const char *const fatal[] = {
        "Permission denied",
        "Host key verification failed",
        "REMOTE HOST IDENTIFICATION HAS CHANGED",
        "Too many authentication failures",
        "no matching host key type",
        "Bad configuration option",
        "Bad port",
    };
    for (const QString &line : log_) {
        for (const char *pattern : fatal) {
            if (line.contains(QLatin1String(pattern), Qt::CaseInsensitive))
                return true;
        }
    }
    return false;
}
