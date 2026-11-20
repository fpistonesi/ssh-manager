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
// thread, which keeps the window responsive and the protocol simple. File
// data is pipelined (several READ/WRITE requests in flight).
//
// A transfer window uses one session for browsing and file operations and
// one per parallel transfer; all of them share the plan's temporary files,
// which the window deletes (setOwnsFiles(false)).
class SftpSession : public QObject {
    Q_OBJECT

public:
    explicit SftpSession(QObject *parent = nullptr);
    ~SftpSession() override;

    void configure(const SftpPlan &plan);
    void setOwnsFiles(bool owns) { ownsFiles_ = owns; }

    // Thread-safe: called from the window while the worker is copying.
    void requestCancel();

public slots:
    void open();
    void listDirectory(const QString &path);
    void close();

    // One queued transfer: `source` (a file or a directory) is copied into
    // `destinationDir`. With `resume`, files already complete are skipped and
    // partial ones continue where they stopped.
    void runJob(int jobId, bool upload, const QString &source, const QString &destinationDir,
                bool resume);

    // Remote file operations; each ends with operationFinished().
    void makeDirectory(const QString &path);
    void renamePath(const QString &from, const QString &to);
    void removePaths(const QStringList &paths);
    void setPermissions(const QStringList &paths, quint32 mode);

    // Editing a remote file with a local program.
    void fetchForEdit(const QString &remotePath, const QString &localPath);
    void storeFromEdit(const QString &localPath, const QString &remotePath);

signals:
    void opened(const QString &homePath);
    void listed(const QString &path, const QVector<RemoteEntry> &entries);
    void failed(const QString &error);

    void jobPlanned(int jobId, qint64 total, int files);
    void jobProgress(int jobId, qint64 done, qint64 total, const QString &current);
    // `connectionLost`: the failure came from the connection, not from the
    // files (worth an automatic retry with resume).
    void jobFinished(int jobId, bool ok, const QString &error, bool cancelled, bool connectionLost);

    void operationFinished(const QString &refreshDir, bool ok, const QString &error);
    void editFetched(const QString &remotePath, const QString &localPath, bool ok,
                     const QString &error);
    void editStored(const QString &remotePath, bool ok, const QString &error);

private:
    bool ensureOpen(QString *error);
    bool sendRequest(quint8 type, const QByteArray &body, quint32 *id, QString *error);
    bool receiveResponse(quint32 *id, quint8 *type, QByteArray *payload, QString *error);
    bool statusCall(quint8 type, const QByteArray &body, QString *error);
    // kind: 1 a real directory, 0 a file or link, -1 unknown (looked up).
    bool removeTree(const QString &path, int kind, QString *error);
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

    bool buildUploadPlan(const QStringList &sources, const QString &destination,
                         QVector<TransferItem> *items, qint64 *total, QString *error);
    bool buildDownloadPlan(const QStringList &sources, const QString &destination,
                           QVector<TransferItem> *items, qint64 *total, QString *error);
    bool scanLocal(const QString &localDir, const QString &remoteDir, int rootIndex,
                   QVector<TransferItem> *items, qint64 *total, QString *error);
    bool scanRemote(const QString &remoteDir, const QString &localDir, int rootIndex,
                    QVector<TransferItem> *items, qint64 *total, QString *error);

    bool uploadFile(const TransferItem &item, bool resume, int jobId, qint64 *done, qint64 total,
                    QString *error);
    bool downloadFile(const TransferItem &item, bool resume, int jobId, qint64 *done,
                      qint64 total, QString *error);
    void emitProgress(int jobId, qint64 done, qint64 total, const QString &current);

    SftpPlan plan_;
    QProcess *process_ = nullptr;
    QByteArray buffer_;
    QString stderrTail_;
    quint32 lastId_ = 0;
    QString home_;
    bool opened_ = false;
    bool ownsFiles_ = true;
    bool connectionError_ = false; // the last failure came from the connection
    std::atomic<bool> cancel_{false};
    QElapsedTimer progressTimer_;
};

// Suffix of downloads in progress; renamed to the final name when complete,
// kept after a failure so that a retry can resume.
inline QString partialSuffix()
{
    return QStringLiteral(".part");
}
