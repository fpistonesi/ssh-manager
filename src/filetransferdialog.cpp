#include "filetransferdialog.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStyle>
#include <QTableWidget>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

namespace {

constexpr int kProgressSteps = 1000;

QString formatSize(qint64 bytes)
{
    if (bytes < 1024)
        return FileTransferDialog::tr("%1 B").arg(bytes);
    const double kib = bytes / 1024.0;
    if (kib < 1024)
        return QStringLiteral("%1 KB").arg(QString::number(kib, 'f', kib < 10 ? 1 : 0));
    const double mib = kib / 1024.0;
    if (mib < 1024)
        return QStringLiteral("%1 MB").arg(QString::number(mib, 'f', mib < 10 ? 1 : 0));
    return QStringLiteral("%1 GB").arg(QString::number(mib / 1024.0, 'f', 2));
}

QString permissionText(quint32 mode, bool isDir, bool isLink)
{
    if (mode == 0)
        return QStringLiteral("—");
    QString text = isLink ? QStringLiteral("l") : (isDir ? QStringLiteral("d") : QStringLiteral("-"));
    const quint32 bits = mode & 0777;
    for (int bit = 8; bit >= 0; --bit) {
        const int position = 8 - bit;
        const quint32 mask = 1u << bit;
        QChar letter = QLatin1Char('-');
        if (bits & mask) {
            if (position % 3 == 0)
                letter = QLatin1Char('r');
            else if (position % 3 == 1)
                letter = QLatin1Char('w');
            else
                letter = QLatin1Char('x');
        }
        text.append(letter);
    }
    return text;
}

QString parentPath(const QString &path)
{
    QString trimmed = path;
    while (trimmed.size() > 1 && trimmed.endsWith(QLatin1Char('/')))
        trimmed.chop(1);
    const int slash = trimmed.lastIndexOf(QLatin1Char('/'));
    if (slash <= 0)
        return QStringLiteral("/");
    return trimmed.left(slash);
}

} // namespace

FileTransferDialog::FileTransferDialog(const Connection &connection, const SftpPlan &plan,
                                       QWidget *parent)
    : QDialog(parent), connection_(connection), plan_(plan)
{
    qRegisterMetaType<QVector<RemoteEntry>>("QVector<RemoteEntry>");
    qRegisterMetaType<QVector<TransferItem>>("QVector<TransferItem>");

    setWindowTitle(tr("Trasferimento file — %1").arg(connection_.displayName()));
    setModal(true);
    resize(1120, 720);

    buildUi();
    startSession();
}

FileTransferDialog::~FileTransferDialog()
{
    finishSession();
}

void FileTransferDialog::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    auto *title = new QLabel(tr("<b>%1</b> — %2").arg(connection_.displayName(), connection_.target()),
                             this);
    statusLabel_ = new QLabel(tr("Connessione in corso…"), this);
    statusLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *header = new QHBoxLayout;
    header->addWidget(title);
    header->addStretch();
    header->addWidget(statusLabel_);
    layout->addLayout(header);

    auto *splitter = new QSplitter(Qt::Horizontal, this);

    // Local pane.
    auto *localBox = new QGroupBox(tr("Locale"), splitter);
    auto *localLayout = new QVBoxLayout(localBox);
    auto *localBar = new QHBoxLayout;

    auto *localUp = new QToolButton(localBox);
    localUp->setIcon(QIcon::fromTheme(QStringLiteral("go-up"),
                                      style()->standardIcon(QStyle::SP_ArrowUp)));
    localUp->setToolTip(tr("Cartella superiore"));
    localUp->setAutoRaise(true);

    localPathEdit_ = new QLineEdit(localBox);
    localPathEdit_->setObjectName(QStringLiteral("localPath"));
    localPathEdit_->setClearButtonEnabled(false);

    localModel_ = new QFileSystemModel(this);
    localModel_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    localModel_->setRootPath(QDir::rootPath());

    localView_ = new QTreeView(localBox);
    localView_->setObjectName(QStringLiteral("localView"));
    localView_->setModel(localModel_);
    localView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    localView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    localView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    localView_->setUniformRowHeights(true);
    localView_->setSortingEnabled(true);
    localView_->sortByColumn(0, Qt::AscendingOrder);
    for (int column = 1; column < localModel_->columnCount(); ++column)
        localView_->hideColumn(column);
    localView_->header()->setStretchLastSection(true);

    localBar->addWidget(localUp);
    localBar->addWidget(localPathEdit_, 1);
    localLayout->addLayout(localBar);
    localLayout->addWidget(localView_, 1);

    // Remote pane.
    auto *remoteBox = new QGroupBox(tr("Remoto"), splitter);
    auto *remoteLayout = new QVBoxLayout(remoteBox);
    auto *remoteBar = new QHBoxLayout;

    auto *remoteUp = new QToolButton(remoteBox);
    remoteUp->setIcon(QIcon::fromTheme(QStringLiteral("go-up"),
                                       style()->standardIcon(QStyle::SP_ArrowUp)));
    remoteUp->setToolTip(tr("Cartella superiore"));
    remoteUp->setAutoRaise(true);

    auto *remoteHome = new QToolButton(remoteBox);
    remoteHome->setIcon(QIcon::fromTheme(QStringLiteral("go-home"),
                                         style()->standardIcon(QStyle::SP_DirHomeIcon)));
    remoteHome->setToolTip(tr("Torna alla home remota"));
    remoteHome->setAutoRaise(true);

    remotePathEdit_ = new QLineEdit(remoteBox);
    remotePathEdit_->setObjectName(QStringLiteral("remotePath"));

    remoteModel_ = new QStandardItemModel(0, 4, this);
    remoteModel_->setHorizontalHeaderLabels(
        {tr("Nome"), tr("Dimensione"), tr("Modificato"), tr("Permessi")});

    remoteView_ = new QTreeView(remoteBox);
    remoteView_->setObjectName(QStringLiteral("remoteView"));
    remoteView_->setModel(remoteModel_);
    remoteView_->setRootIsDecorated(false);
    remoteView_->setUniformRowHeights(true);
    remoteView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    remoteView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    remoteView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    remoteView_->setSortingEnabled(false);
    remoteView_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 4; ++column)
        remoteView_->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);

    remoteBar->addWidget(remoteUp);
    remoteBar->addWidget(remoteHome);
    remoteBar->addWidget(remotePathEdit_, 1);
    remoteLayout->addLayout(remoteBar);
    remoteLayout->addWidget(remoteView_, 1);

    // Transfer buttons between the panes.
    auto *actions = new QWidget(splitter);
    auto *actionsLayout = new QVBoxLayout(actions);
    actionsLayout->setContentsMargins(4, 0, 4, 0);
    actionsLayout->addStretch();

    uploadButton_ = new QPushButton(tr("Invia →"), actions);
    uploadButton_->setObjectName(QStringLiteral("uploadButton"));
    uploadButton_->setIcon(QIcon::fromTheme(QStringLiteral("go-next"),
                                            style()->standardIcon(QStyle::SP_ArrowRight)));
    uploadButton_->setToolTip(tr("Copia gli elementi locali selezionati nella cartella remota"));
    uploadButton_->setMinimumHeight(34);

    downloadButton_ = new QPushButton(tr("← Ricevi"), actions);
    downloadButton_->setObjectName(QStringLiteral("downloadButton"));
    downloadButton_->setIcon(QIcon::fromTheme(QStringLiteral("go-previous"),
                                              style()->standardIcon(QStyle::SP_ArrowLeft)));
    downloadButton_->setToolTip(tr("Copia gli elementi remoti selezionati nella cartella locale"));
    downloadButton_->setMinimumHeight(34);

    actionsLayout->addWidget(uploadButton_);
    actionsLayout->addWidget(downloadButton_);
    actionsLayout->addStretch();

    splitter->addWidget(localBox);
    splitter->addWidget(actions);
    splitter->addWidget(remoteBox);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    splitter->setStretchFactor(2, 1);
    layout->addWidget(splitter, 1);

    // Transfer queue.
    auto *queueBox = new QGroupBox(tr("Coda di trasferimento"), this);
    auto *queueLayout = new QVBoxLayout(queueBox);

    queue_ = new QTableWidget(0, 4, queueBox);
    queue_->setObjectName(QStringLiteral("transferQueue"));
    queue_->setHorizontalHeaderLabels({tr("Elemento"), tr("Direzione"), tr("Dimensione"), tr("Stato")});
    queue_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 4; ++column)
        queue_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    queue_->verticalHeader()->setVisible(false);
    queue_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    queue_->setSelectionMode(QAbstractItemView::NoSelection);
    queue_->setAlternatingRowColors(true);
    queue_->setMinimumHeight(96);
    queue_->setMaximumHeight(160);
    queueLayout->addWidget(queue_);

    progressBar_ = new QProgressBar(queueBox);
    progressBar_->setObjectName(QStringLiteral("transferProgress"));
    progressBar_->setRange(0, kProgressSteps);
    progressBar_->setValue(0);
    progressBar_->setTextVisible(true);
    queueLayout->addWidget(progressBar_);

    layout->addWidget(queueBox);

    auto *bottom = new QHBoxLayout;
    bottom->addStretch();
    cancelButton_ = new QPushButton(tr("Annulla trasferimento"), this);
    cancelButton_->setIcon(style()->standardIcon(QStyle::SP_BrowserStop));
    closeButton_ = new QPushButton(tr("Chiudi"), this);
    closeButton_->setDefault(true);
    bottom->addWidget(cancelButton_);
    bottom->addWidget(closeButton_);
    layout->addLayout(bottom);

    connect(closeButton_, &QPushButton::clicked, this, &QDialog::reject);
    connect(cancelButton_, &QPushButton::clicked, this, &FileTransferDialog::cancelTransfer);
    connect(uploadButton_, &QPushButton::clicked, this, &FileTransferDialog::uploadSelection);
    connect(downloadButton_, &QPushButton::clicked, this, &FileTransferDialog::downloadSelection);

    connect(localView_, &QTreeView::doubleClicked, this, &FileTransferDialog::onLocalActivated);
    connect(remoteView_, &QTreeView::doubleClicked, this, &FileTransferDialog::onRemoteActivated);
    connect(localView_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { updateControls(); });
    connect(remoteView_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { updateControls(); });

    connect(localUp, &QToolButton::clicked, this, [this] { navigateLocal(parentPath(localDir_)); });
    connect(remoteUp, &QToolButton::clicked, this,
            [this] { navigateRemote(parentPath(remoteDir_)); });
    connect(remoteHome, &QToolButton::clicked, this, [this] {
        if (!remoteHome_.isEmpty())
            navigateRemote(remoteHome_);
    });
    connect(localPathEdit_, &QLineEdit::returnPressed, this,
            [this] { navigateLocal(localPathEdit_->text()); });
    connect(remotePathEdit_, &QLineEdit::returnPressed, this,
            [this] { navigateRemote(remotePathEdit_->text()); });

    navigateLocal(QDir::homePath());
    updateControls();
}

void FileTransferDialog::startSession()
{
    if (!plan_.valid) {
        statusLabel_->setText(plan_.error);
        QMessageBox::critical(this, tr("Trasferimento file"), plan_.error);
        return;
    }

    session_ = new SftpSession;
    session_->configure(plan_);
    session_->moveToThread(&thread_);

    connect(&thread_, &QThread::finished, session_, &QObject::deleteLater);
    connect(session_, &SftpSession::opened, this, &FileTransferDialog::onOpened);
    connect(session_, &SftpSession::listed, this, &FileTransferDialog::onListed);
    connect(session_, &SftpSession::failed, this, &FileTransferDialog::onFailed);
    connect(session_, &SftpSession::planReady, this, &FileTransferDialog::onPlanReady);
    connect(session_, &SftpSession::progress, this, &FileTransferDialog::onProgress);
    connect(session_, &SftpSession::transferFinished, this,
            &FileTransferDialog::onTransferFinished);

    thread_.start();
    QMetaObject::invokeMethod(session_, "open", Qt::QueuedConnection);
}

void FileTransferDialog::finishSession()
{
    if (!session_)
        return;
    session_->requestCancel();
    QMetaObject::invokeMethod(session_, "close", Qt::BlockingQueuedConnection);
    thread_.quit();
    thread_.wait(5000);
    session_ = nullptr;
}

void FileTransferDialog::navigateLocal(const QString &path)
{
    QString clean = QDir::cleanPath(path.trimmed());
    const QFileInfo info(clean);
    if (!info.isDir())
        clean = info.absolutePath();

    localDir_ = clean;
    localView_->setRootIndex(localModel_->index(clean));
    localPathEdit_->setText(QDir::toNativeSeparators(clean));
    updateControls();
}

void FileTransferDialog::navigateRemote(const QString &path)
{
    if (!connected_ || path.trimmed().isEmpty())
        return;

    remoteDir_ = path.trimmed();
    remotePathEdit_->setText(remoteDir_);
    statusLabel_->setText(tr("Lettura di %1…").arg(remoteDir_));
    QMetaObject::invokeMethod(session_, "listDirectory", Qt::QueuedConnection,
                              Q_ARG(QString, remoteDir_));
}

void FileTransferDialog::onLocalActivated(const QModelIndex &index)
{
    if (!index.isValid() || !localModel_->isDir(index))
        return;
    navigateLocal(localModel_->filePath(index));
}

void FileTransferDialog::onRemoteActivated(const QModelIndex &index)
{
    if (!index.isValid() || index.column() != 0)
        return;
    QStandardItem *item = remoteModel_->itemFromIndex(index);
    if (!item || !item->data(Qt::UserRole + 1).toBool())
        return;
    navigateRemote(item->data(Qt::UserRole).toString());
}

void FileTransferDialog::onOpened(const QString &home)
{
    connected_ = true;
    remoteHome_ = home;
    statusLabel_->setText(tr("Connesso a %1").arg(connection_.target()));
    navigateRemote(home);
    updateControls();
}

void FileTransferDialog::onListed(const QString &path, const QVector<RemoteEntry> &entries)
{
    if (path != remoteDir_)
        return;

    remoteModel_->removeRows(0, remoteModel_->rowCount());
    for (const RemoteEntry &entry : entries) {
        auto *nameItem = new QStandardItem(style()->standardIcon(
                                               entry.isLink ? QStyle::SP_FileLinkIcon
                                                            : (entry.isDir ? QStyle::SP_DirIcon
                                                                           : QStyle::SP_FileIcon)),
                                           entry.name);
        nameItem->setEditable(false);
        nameItem->setData(entry.path, Qt::UserRole);
        nameItem->setData(entry.isDir, Qt::UserRole + 1);
        nameItem->setToolTip(entry.path);

        auto *sizeItem = new QStandardItem(entry.isDir ? QStringLiteral("—")
                                                       : formatSize(entry.size));
        sizeItem->setEditable(false);
        sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sizeItem->setData(entry.size, Qt::UserRole);

        auto *dateItem = new QStandardItem(
            entry.modified.isValid() ? entry.modified.toString(QStringLiteral("dd/MM/yyyy HH:mm"))
                                     : QStringLiteral("—"));
        dateItem->setEditable(false);

        auto *modeItem = new QStandardItem(permissionText(entry.mode, entry.isDir, entry.isLink));
        modeItem->setEditable(false);

        remoteModel_->appendRow({nameItem, sizeItem, dateItem, modeItem});
    }

    statusLabel_->setText(tr("%1 elementi in %2").arg(entries.size()).arg(path));
    updateControls();
}

void FileTransferDialog::onFailed(const QString &error)
{
    statusLabel_->setText(error);
    if (!connected_) {
        QMessageBox::critical(this, tr("Trasferimento file"), error);
        updateControls();
        return;
    }
    QMessageBox::warning(this, tr("Cartella remota"), error);
}

void FileTransferDialog::onPlanReady(const QVector<TransferItem> &items)
{
    QVector<int> fileCount(queue_->rowCount(), 0);
    QVector<qint64> sizes(queue_->rowCount(), 0);

    for (const TransferItem &item : items) {
        if (item.directory || item.rootIndex < 0 || item.rootIndex >= fileCount.size())
            continue;
        fileCount[item.rootIndex] += 1;
        sizes[item.rootIndex] += item.size;
    }

    qint64 total = 0;
    for (int row = 0; row < queue_->rowCount(); ++row) {
        const qint64 size = sizes.at(row);
        total += size;
        QTableWidgetItem *sizeItem = queue_->item(row, 2);
        if (!sizeItem)
            continue;
        QString text = formatSize(size);
        if (fileCount.at(row) > 1)
            text = tr("%1 file · %2").arg(fileCount.at(row)).arg(text);
        sizeItem->setText(text);
    }

    progressBar_->setValue(0);
    statusLabel_->setText(total > 0 ? tr("Trasferimento di %1…").arg(formatSize(total))
                                    : tr("Trasferimento in corso…"));
}

void FileTransferDialog::onProgress(qint64 done, qint64 total, int rootIndex,
                                    const QString &current)
{
    const int value = total > 0 ? int((done * kProgressSteps) / total) : 0;
    progressBar_->setValue(qBound(0, value, kProgressSteps));

    if (rootIndex >= 0 && rootIndex < queue_->rowCount()) {
        if (QTableWidgetItem *state = queue_->item(rootIndex, 3)) {
            if (state->text() == tr("In attesa"))
                state->setText(tr("In corso"));
        }
    }

    if (!current.isEmpty()) {
        statusLabel_->setText(tr("%1 — %2 di %3")
                                  .arg(current, formatSize(done),
                                       total > 0 ? formatSize(total) : QStringLiteral("?")));
    }
}

void FileTransferDialog::onTransferFinished(bool ok, const QString &error, bool cancelled)
{
    busy_ = false;

    for (int row = 0; row < queue_->rowCount(); ++row) {
        QTableWidgetItem *state = queue_->item(row, 3);
        if (!state)
            continue;
        if (state->text() == tr("In corso"))
            state->setText(ok ? tr("Completato") : (cancelled ? tr("Annullato") : tr("Errore")));
        else if (state->text() == tr("In attesa"))
            state->setText(ok ? tr("Completato") : tr("Annullato"));
    }

    if (ok) {
        progressBar_->setValue(progressBar_->maximum());
        statusLabel_->setText(tr("Trasferimento completato."));
    } else {
        statusLabel_->setText(error);
        if (!cancelled)
            QMessageBox::warning(this, tr("Trasferimento file"), error);
    }

    if (cancelled) {
        // The worker drops the connection on cancellation, so the session is gone.
        connected_ = false;
        statusLabel_->setText(tr("Trasferimento annullato. Chiudi e riapri la finestra per "
                                 "riconnettersi."));
    } else if (connected_ && !remoteDir_.isEmpty()) {
        navigateRemote(remoteDir_);
        navigateLocal(localDir_);
    }

    updateControls();
}

void FileTransferDialog::uploadSelection()
{
    if (!connected_ || busy_ || remoteDir_.isEmpty())
        return;
    const QStringList sources = selectedLocalPaths();
    if (sources.isEmpty())
        return;
    queueTransfer(true, sources);
}

void FileTransferDialog::downloadSelection()
{
    if (!connected_ || busy_ || localDir_.isEmpty())
        return;
    const QStringList sources = selectedRemotePaths();
    if (sources.isEmpty())
        return;
    queueTransfer(false, sources);
}

void FileTransferDialog::queueTransfer(bool upload, const QStringList &sources)
{
    queue_->setRowCount(0);
    progressBar_->setValue(0);

    for (int i = 0; i < sources.size(); ++i) {
        const QFileInfo info(sources.at(i));
        const QString name =
            upload ? info.fileName() : sources.at(i).section(QLatin1Char('/'), -1);
        const bool directory = upload ? info.isDir() : remoteDirectory(sources.at(i));

        auto *nameItem = new QTableWidgetItem(name);
        nameItem->setIcon(style()->standardIcon(directory ? QStyle::SP_DirIcon
                                                          : QStyle::SP_FileIcon));
        nameItem->setToolTip(sources.at(i));

        auto *direction = new QTableWidgetItem(upload ? tr("Invio") : tr("Ricezione"));

        const qint64 size = directory ? -1 : (upload ? info.size() : remoteSize(sources.at(i)));
        auto *sizeItem = new QTableWidgetItem(size < 0 ? tr("cartella") : formatSize(size));
        sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

        auto *stateItem = new QTableWidgetItem(tr("In attesa"));

        queue_->insertRow(i);
        queue_->setItem(i, 0, nameItem);
        queue_->setItem(i, 1, direction);
        queue_->setItem(i, 2, sizeItem);
        queue_->setItem(i, 3, stateItem);
    }

    busy_ = true;
    updateControls();
    statusLabel_->setText(upload ? tr("Preparazione dell'invio…")
                                 : tr("Preparazione della ricezione…"));

    QMetaObject::invokeMethod(session_, "transfer", Qt::QueuedConnection, Q_ARG(bool, upload),
                              Q_ARG(QStringList, sources),
                              Q_ARG(QString, upload ? remoteDir_ : localDir_));
}

void FileTransferDialog::cancelTransfer()
{
    if (!busy_ || !session_)
        return;
    session_->requestCancel();
    statusLabel_->setText(tr("Annullamento in corso…"));
    cancelButton_->setEnabled(false);
}

void FileTransferDialog::updateControls()
{
    const bool idle = connected_ && !busy_;
    localView_->setEnabled(idle);
    remoteView_->setEnabled(idle);
    localPathEdit_->setEnabled(idle);
    remotePathEdit_->setEnabled(idle);
    uploadButton_->setEnabled(idle && !selectedLocalPaths().isEmpty());
    downloadButton_->setEnabled(idle && !selectedRemotePaths().isEmpty());
    cancelButton_->setEnabled(busy_);
    closeButton_->setEnabled(!busy_);
}

QStringList FileTransferDialog::selectedLocalPaths() const
{
    QStringList paths;
    if (!localView_ || !localView_->selectionModel())
        return paths;
    const QModelIndexList indexes = localView_->selectionModel()->selectedIndexes();
    for (const QModelIndex &index : indexes) {
        if (index.column() != 0)
            continue;
        const QString path = localModel_->filePath(index);
        if (!path.isEmpty() && !paths.contains(path))
            paths.append(path);
    }
    return paths;
}

QStringList FileTransferDialog::selectedRemotePaths() const
{
    QStringList paths;
    if (!remoteView_ || !remoteView_->selectionModel())
        return paths;
    const QModelIndexList indexes = remoteView_->selectionModel()->selectedIndexes();
    for (const QModelIndex &index : indexes) {
        if (index.column() != 0)
            continue;
        const QStandardItem *item = remoteModel_->itemFromIndex(index);
        if (!item)
            continue;
        const QString path = item->data(Qt::UserRole).toString();
        if (!path.isEmpty() && !paths.contains(path))
            paths.append(path);
    }
    return paths;
}

qint64 FileTransferDialog::remoteSize(const QString &path) const
{
    for (int row = 0; row < remoteModel_->rowCount(); ++row) {
        const QStandardItem *item = remoteModel_->item(row, 0);
        if (item && item->data(Qt::UserRole).toString() == path) {
            if (item->data(Qt::UserRole + 1).toBool())
                return -1;
            const QStandardItem *sizeItem = remoteModel_->item(row, 1);
            return sizeItem ? sizeItem->data(Qt::UserRole).toLongLong() : 0;
        }
    }
    return 0;
}

bool FileTransferDialog::remoteDirectory(const QString &path) const
{
    for (int row = 0; row < remoteModel_->rowCount(); ++row) {
        const QStandardItem *item = remoteModel_->item(row, 0);
        if (item && item->data(Qt::UserRole).toString() == path)
            return item->data(Qt::UserRole + 1).toBool();
    }
    return false;
}
