#include "hostprobe.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QTcpSocket>
#include <QTimer>

#include <memory>

HostProbe::HostProbe(QObject *parent) : QObject(parent) {}

bool HostProbe::isChecking(const QString &key) const
{
    return running_.contains(key);
}

void HostProbe::check(const QString &key, const QString &host, quint16 port, int timeoutMs)
{
    if (running_.contains(key))
        return;
    running_ << key;

    auto *socket = new QTcpSocket(this);
    auto *timer = new QTimer(socket);
    timer->setSingleShot(true);
    auto clock = std::make_shared<QElapsedTimer>();
    auto latency = std::make_shared<int>(-1);
    auto done = std::make_shared<bool>(false);

    // Reports once, then disposes of the socket.
    const auto finish = [this, key, socket, done](Result::State state, int latencyMs,
                                                  const QString &banner, const QString &error) {
        if (*done)
            return;
        *done = true;
        running_.removeAll(key);
        Result result;
        result.state = state;
        result.latencyMs = latencyMs;
        result.banner = banner;
        result.error = error;
        result.checkedAt = QDateTime::currentDateTime();
        socket->abort();
        socket->deleteLater();
        emit this->result(key, result);
    };

    connect(socket, &QTcpSocket::connected, this, [socket, clock, latency, timer] {
        *latency = int(clock->elapsed());
        // The server speaks first: wait briefly for its identification line.
        timer->start(2000);
        Q_UNUSED(socket)
    });
    connect(socket, &QTcpSocket::readyRead, this, [socket, latency, finish] {
        if (!socket->canReadLine() && socket->bytesAvailable() < 255)
            return;
        QString line = QString::fromLatin1(socket->readLine(255)).trimmed();
        if (line.startsWith(QLatin1String("SSH-")))
            line = line.section(QLatin1Char('-'), 2); // drop "SSH-2.0-"
        finish(Result::State::Reachable, *latency, line, QString());
    });
    connect(socket, &QTcpSocket::errorOccurred, this, [socket, latency, finish](QAbstractSocket::SocketError) {
        if (*latency >= 0) // connected, then closed: still reachable
            finish(Result::State::Reachable, *latency, QString(), QString());
        else
            finish(Result::State::Unreachable, -1, QString(), socket->errorString());
    });
    connect(timer, &QTimer::timeout, this, [latency, finish] {
        if (*latency >= 0)
            finish(Result::State::Reachable, *latency, QString(), QString()); // no banner in time
        else
            finish(Result::State::Unreachable, -1, QString(), tr("Nessuna risposta (timeout)"));
    });

    clock->start();
    timer->start(timeoutMs);
    socket->connectToHost(host, port);
}
