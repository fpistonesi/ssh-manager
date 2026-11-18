#pragma once

#include "model.h"
#include "sftpclient.h"

#include <QDialog>
#include <QThread>

class QFileSystemModel;
class QLabel;
class QLineEdit;
class QModelIndex;
class QProgressBar;
class QPushButton;
class QStandardItemModel;
class QTableWidget;
class QTreeView;

// Two-pane transfer window: local files on the left, the remote directory on
// the right, with an upload/download queue and byte progress.
class FileTransferDialog : public QDialog {
    Q_OBJECT

public:
    FileTransferDialog(const Connection &connection, const SftpPlan &plan,
                       QWidget *parent = nullptr);
    ~FileTransferDialog() override;

private:
    void buildUi();
    void startSession();
    void finishSession();

    void navigateLocal(const QString &path);
    void navigateRemote(const QString &path);
    void onLocalActivated(const QModelIndex &index);
    void onRemoteActivated(const QModelIndex &index);

    void onOpened(const QString &home);
    void onListed(const QString &path, const QVector<RemoteEntry> &entries);
    void onFailed(const QString &error);
    void onPlanReady(const QVector<TransferItem> &items);
    void onProgress(qint64 done, qint64 total, int rootIndex, const QString &current);
    void onTransferFinished(bool ok, const QString &error, bool cancelled);

    void uploadSelection();
    void downloadSelection();
    void queueTransfer(bool upload, const QStringList &sources);
    void cancelTransfer();
    void updateControls();

    QStringList selectedLocalPaths() const;
    QStringList selectedRemotePaths() const;
    qint64 remoteSize(const QString &path) const;
    bool remoteDirectory(const QString &path) const;

    Connection connection_;
    SftpPlan plan_;
    QThread thread_;
    SftpSession *session_ = nullptr;

    QFileSystemModel *localModel_ = nullptr;
    QTreeView *localView_ = nullptr;
    QLineEdit *localPathEdit_ = nullptr;
    QStandardItemModel *remoteModel_ = nullptr;
    QTreeView *remoteView_ = nullptr;
    QLineEdit *remotePathEdit_ = nullptr;
    QTableWidget *queue_ = nullptr;
    QProgressBar *progressBar_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QPushButton *uploadButton_ = nullptr;
    QPushButton *downloadButton_ = nullptr;
    QPushButton *cancelButton_ = nullptr;
    QPushButton *closeButton_ = nullptr;

    QString localDir_;
    QString remoteDir_;
    QString remoteHome_;
    bool connected_ = false;
    bool busy_ = false;
};
