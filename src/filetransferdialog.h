#pragma once

#include "model.h"
#include "sftpclient.h"

#include <QDateTime>
#include <QDialog>
#include <QHash>
#include <QVector>

class QFileSystemModel;
class QFileSystemWatcher;
class QLabel;
class QLineEdit;
class QMenu;
class QModelIndex;
class QPushButton;
class QSpinBox;
class QStandardItemModel;
class QTableWidget;
class QThread;
class QTimer;
class QToolButton;
class QTreeView;

// Two-pane transfer window: local files on the left, the remote directory on
// the right. Transfers go to a queue served by parallel SFTP connections, so
// browsing and file operations stay available while files are copied.
class FileTransferDialog : public QDialog {
    Q_OBJECT

public:
    FileTransferDialog(const Connection &connection, const SftpPlan &plan,
                       QWidget *parent = nullptr);
    ~FileTransferDialog() override;

signals:
    // The remote bookmarks of this connection changed (saved in the vault).
    void remoteBookmarksChanged(const QString &connectionId, const QStringList &bookmarks);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void reject() override;

private:
    struct Job {
        enum class State { Queued, Running, Done, Failed, Cancelled };
        int id = 0;
        bool upload = true;
        QString source;
        QString destinationDir;
        QString name;
        bool directory = false;
        State state = State::Queued;
        bool resume = false;
        int attempts = 0;
        qint64 done = 0;
        qint64 total = -1;
        int files = 0;
        QString error;
        QString current;
    };
    struct Worker {
        QThread *thread = nullptr;
        SftpSession *session = nullptr;
        int jobId = -1;
    };
    struct EditedFile {
        QString remotePath;
        QString localPath;
        QDateTime uploadedStamp; // local modification time last sent
        qint64 uploadedSize = -1;
        bool uploading = false;
        bool again = false;      // changed again while uploading
    };

    void buildUi();
    void startBrowser();
    void stopSessions();

    void navigateLocal(const QString &path);
    void navigateRemote(const QString &path);
    void refreshRemote();

    // Queue.
    void enqueue(bool upload, const QStringList &sources, const QString &destinationDir);
    void dispatch();
    Worker *createWorker();
    void onJobPlanned(int jobId, qint64 total, int files);
    void onJobProgress(int jobId, qint64 done, qint64 total, const QString &current);
    void onJobFinished(int jobId, bool ok, const QString &error, bool cancelled,
                       bool connectionLost);
    void cancelJobs(const QList<int> &ids);
    void retryJobs(const QList<int> &ids);
    void removeJobs(const QList<int> &ids);
    void updateJobRow(const Job &job);
    int rowOfJob(int id) const;
    Job *job(int id);
    QList<int> selectedJobIds() const;
    bool hasActiveJobs() const;
    void updateSummary();

    // Remote operations.
    void remoteContextMenu(const QPoint &pos);
    void remoteNewFolder();
    void remoteRename();
    void remoteDelete();
    void remotePermissions();
    void remoteEdit();
    void onOperationFinished(const QString &refreshDir, bool ok, const QString &error);

    // Local operations.
    void localContextMenu(const QPoint &pos);
    void localNewFolder();
    void localRename();
    void localDelete();

    // Editing remote files locally.
    void onEditFetched(const QString &remotePath, const QString &localPath, bool ok,
                       const QString &error);
    void onEditStored(const QString &remotePath, bool ok, const QString &error);
    void onEditedFileTouched(const QString &path);
    void uploadEditedFile(const QString &localPath);

    // Bookmarks.
    void showBookmarks(bool remote);

    void onOpened(const QString &home);
    void onListed(const QString &path, const QVector<RemoteEntry> &entries);
    void onFailed(const QString &error);
    void updateControls();
    void setStatus(const QString &text);

    QStringList selectedLocalPaths() const;
    QStringList selectedRemotePaths() const;
    QVector<RemoteEntry> selectedRemoteEntries() const;
    QString remoteDropTarget(const QPoint &viewportPos) const;
    QString localDropTarget(const QPoint &viewportPos) const;

    Connection connection_;
    SftpPlan plan_;

    QThread *browserThread_ = nullptr;
    SftpSession *browser_ = nullptr;
    QVector<Worker *> workers_;
    QVector<Job> jobs_;
    int nextJobId_ = 1;
    int maxParallel_ = 2;

    QHash<QString, EditedFile> edited_; // by local path
    QFileSystemWatcher *editWatcher_ = nullptr;
    QHash<QString, QTimer *> editTimers_;
    QString editRoot_; // temporary copies of files being edited

    QFileSystemModel *localModel_ = nullptr;
    QTreeView *localView_ = nullptr;
    QLineEdit *localPathEdit_ = nullptr;
    QStandardItemModel *remoteModel_ = nullptr;
    QTreeView *remoteView_ = nullptr;
    QLineEdit *remotePathEdit_ = nullptr;
    QToolButton *localBookmarks_ = nullptr;
    QToolButton *remoteBookmarks_ = nullptr;
    QTableWidget *queue_ = nullptr;
    QSpinBox *parallel_ = nullptr;
    QLabel *summaryLabel_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QPushButton *uploadButton_ = nullptr;
    QPushButton *downloadButton_ = nullptr;

    QString localDir_;
    QString remoteDir_;
    QString remoteHome_;
    QVector<RemoteEntry> remoteEntries_;
    bool connected_ = false;
};
