#pragma once

#include "model.h"
#include "sshcommand.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>

class QProcess;

// One remote directory entry, as reported by the SFTP server.
struct RemoteEntry {
    QString name;
    QString path;
    bool isDir = false;
    bool isLink = false;
    qint64 size = 0;
    quint32 mode = 0;
    QDateTime modified;
};

// One step of a transfer: a file to copy, or a directory to create first.
struct TransferItem {
    int rootIndex = 0;  // index of the selected item the step belongs to
    bool upload = true;
    bool directory = false;
    QString source;  // local path for uploads, remote path for downloads
    QString target;  // remote path for uploads, local path for downloads
    qint64 size = 0;
};

Q_DECLARE_METATYPE(RemoteEntry)
Q_DECLARE_METATYPE(QVector<RemoteEntry>)
Q_DECLARE_METATYPE(TransferItem)
Q_DECLARE_METATYPE(QVector<TransferItem>)

// Speaks the SFTP v3 protocol over "ssh -s <host> sftp", so transfers reuse the
// same authentication as terminal sessions (askpass helper, embedded keys,
// jump hosts, known_hosts). Every method blocks: the object lives in a worker
// thread, which keeps the window responsive and the protocol simple.
class SftpSession : public QObject {
    Q_OBJECT

public:
    explicit SftpSession(QObject *parent = nullptr);
    ~SftpSession() override;

    void configure(const SftpPlan &plan);

    // Thread-safe: called from the window while the worker is copying.
    void requestCancel();

public slots:
    void open();
    void listDirectory(const QString &path);
    void transfer(bool upload, const QStringList &sources, const QString &destination);
    void close();

signals:
    void opened(const QString &homePath);
    void listed(const QString &path, const QVector<RemoteEntry> &entries);
    void failed(const QString &error);
    void planReady(const QVector<TransferItem> &items);
    void progress(qint64 done, qint64 total, int rootIndex, const QString &current);
    void transferFinished(bool ok, const QString &error, bool cancelled);

private:
    bool startProcess(QString *error);
    void stopProcess();
    void cleanupFiles();

    bool sendPacket(quint8 type, const QByteArray &payload, QString *error);
    bool receivePacket(quint8 *type, QByteArray *payload, QString *error);
    bool waitForBytes(int count, QString *error);
    bool call(quint8 type, const QByteArray &body, quint8 *responseType, QByteArray *response,
              QString *error);

    bool handshake(QString *error);
    bool realPath(const QString &path, QString *out, QString *error);
    bool statEntry(const QString &path, RemoteEntry *entry, QString *error);
    QVector<RemoteEntry> readDirectory(const QString &path, QString *error);
    bool ensureDirectory(const QString &path, QString *error);
    bool removeRemote(const QString &path);
    bool openFile(const QString &path, quint32 flags, quint32 mode, QByteArray *handle,
                  QString *error);
    bool closeFile(const QByteArray &handle, QString *error);
    bool readChunk(const QByteArray &handle, quint64 offset, quint32 length, QByteArray *data,
                   bool *eof, QString *error);
    bool writeChunk(const QByteArray &handle, quint64 offset, const QByteArray &data,
                    QString *error);

    bool buildUploadPlan(const QStringList &sources, const QString &destination,
                         QVector<TransferItem> *items, qint64 *total, QString *error);
    bool buildDownloadPlan(const QStringList &sources, const QString &destination,
                           QVector<TransferItem> *items, qint64 *total, QString *error);
    bool scanLocal(const QString &localDir, const QString &remoteDir, int rootIndex,
                   QVector<TransferItem> *items, qint64 *total, QString *error);
    bool scanRemote(const QString &remoteDir, const QString &localDir, int rootIndex,
                    QVector<TransferItem> *items, qint64 *total, QString *error);

    bool uploadFile(const TransferItem &item, qint64 *done, qint64 total, QString *error);
    bool downloadFile(const TransferItem &item, qint64 *done, qint64 total, QString *error);
    void emitProgress(qint64 done, qint64 total, int rootIndex, const QString &current);

    SftpPlan plan_;
    QProcess *process_ = nullptr;
    QByteArray buffer_;
    QString stderrTail_;
    quint32 lastId_ = 0;
    QString home_;
    bool opened_ = false;
    std::atomic<bool> cancel_{false};
    QElapsedTimer progressTimer_;
};
