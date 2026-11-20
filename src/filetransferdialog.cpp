#include "filetransferdialog.h"

#include "apptheme.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFileSystemWatcher>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLocale>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStyle>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include <utility>

namespace {

constexpr int kProgressSteps = 1000;
constexpr int kMaxAttempts = 3;            // automatic retries after a lost connection
constexpr qint64 kEditWarnSize = 20 << 20; // ask before editing bigger files
const char *const kRemoteMime = "application/x-sshmanager-remote-paths";

// Remote item data.
constexpr int PathRole = Qt::UserRole;
constexpr int DirRole = Qt::UserRole + 1;
constexpr int ModeRole = Qt::UserRole + 2;
constexpr int SizeRole = Qt::UserRole + 3;

// Queue columns.
enum QueueColumn { NameColumn, DirectionColumn, SizeColumn, ProgressColumn, StateColumn };

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
    const char letters[] = {'r', 'w', 'x'};
    for (int bit = 8; bit >= 0; --bit)
        text.append((mode & (1u << bit)) ? QLatin1Char(letters[(8 - bit) % 3]) : QLatin1Char('-'));
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

QString joinRemote(const QString &dir, const QString &name)
{
    return dir.endsWith(QLatin1Char('/')) ? dir + name : dir + QLatin1Char('/') + name;
}

QString baseName(const QString &path)
{
    return path.section(QLatin1Char('/'), -1);
}

#ifdef _WIN32
// Local paths on Windows: "C:/Users" -> "C:/" -> "" (the list of drives).
QString localParentPath(const QString &path)
{
    if (path.isEmpty())
        return QString();
    const QString clean = QDir::cleanPath(path);
    if (QDir(clean).isRoot())
        return QString();
    return QFileInfo(clean).absolutePath();
}
#endif

// Remote rows can be dragged to the local pane: the paths travel in a private
// MIME type, never as file URLs (they are not local files).
class RemoteModel : public QStandardItemModel {
public:
    using QStandardItemModel::QStandardItemModel;

    QStringList mimeTypes() const override { return {QString::fromLatin1(kRemoteMime)}; }

    QMimeData *mimeData(const QModelIndexList &indexes) const override
    {
        QStringList paths;
        for (const QModelIndex &index : indexes) {
            if (index.column() != 0)
                continue;
            const QString path = index.data(PathRole).toString();
            if (!path.isEmpty() && !paths.contains(path))
                paths << path;
        }
        auto *data = new QMimeData;
        data->setData(QString::fromLatin1(kRemoteMime), paths.join(QLatin1Char('\n')).toUtf8());
        return data;
    }

    Qt::DropActions supportedDragActions() const override { return Qt::CopyAction; }
};

// chmod dialog: read/write/execute for owner, group and others, kept in
// sync with an octal field.
bool askPermissions(QWidget *parent, const QString &what, quint32 initial, quint32 *mode)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(FileTransferDialog::tr("Permessi"));
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(what, &dialog));

    auto *grid = new QGridLayout();
    const QStringList columns = {FileTransferDialog::tr("Lettura"), FileTransferDialog::tr("Scrittura"),
                                 FileTransferDialog::tr("Esecuzione")};
    const QStringList rows = {FileTransferDialog::tr("Proprietario"), FileTransferDialog::tr("Gruppo"),
                              FileTransferDialog::tr("Altri")};
    for (int c = 0; c < 3; ++c)
        grid->addWidget(new QLabel(columns.at(c), &dialog), 0, c + 1, Qt::AlignHCenter);
    QCheckBox *boxes[9];
    for (int r = 0; r < 3; ++r) {
        grid->addWidget(new QLabel(rows.at(r), &dialog), r + 1, 0);
        for (int c = 0; c < 3; ++c) {
            boxes[r * 3 + c] = new QCheckBox(&dialog);
            grid->addWidget(boxes[r * 3 + c], r + 1, c + 1, Qt::AlignHCenter);
        }
    }
    layout->addLayout(grid);

    auto *octalRow = new QHBoxLayout();
    octalRow->addWidget(new QLabel(FileTransferDialog::tr("Valore ottale:"), &dialog));
    auto *octal = new QLineEdit(&dialog);
    octal->setValidator(
        new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-7]{3,4}")), octal));
    octal->setMaximumWidth(80);
    octalRow->addWidget(octal);
    octalRow->addStretch();
    layout->addLayout(octalRow);

    quint32 special = initial & 07000;
    const auto fromBoxes = [&] {
        quint32 value = special;
        for (int i = 0; i < 9; ++i) {
            if (boxes[i]->isChecked())
                value |= 1u << (8 - i);
        }
        return value;
    };
    const auto toBoxes = [&](quint32 value) {
        for (int i = 0; i < 9; ++i) {
            const QSignalBlocker blocker(boxes[i]);
            boxes[i]->setChecked(value & (1u << (8 - i)));
        }
    };
    toBoxes(initial);
    octal->setText(QString::number(fromBoxes(), 8).rightJustified(3, QLatin1Char('0')));
    for (QCheckBox *box : boxes) {
        QObject::connect(box, &QCheckBox::toggled, &dialog, [&] {
            const QSignalBlocker blocker(octal);
            octal->setText(QString::number(fromBoxes(), 8).rightJustified(3, QLatin1Char('0')));
        });
    }
    QObject::connect(octal, &QLineEdit::textEdited, &dialog, [&](const QString &text) {
        bool ok = false;
        const quint32 value = text.toUInt(&ok, 8);
        if (ok && text.size() >= 3) {
            special = value & 07000;
            toBoxes(value);
        }
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    *mode = fromBoxes();
    return true;
}

} // namespace

FileTransferDialog::FileTransferDialog(const Connection &connection, const SftpPlan &plan,
                                       QWidget *parent)
    : QDialog(parent), connection_(connection), plan_(plan)
{
    qRegisterMetaType<QVector<RemoteEntry>>("QVector<RemoteEntry>");

    setWindowTitle(tr("Trasferimento file — %1").arg(connection_.displayName()));
    setWindowFlag(Qt::WindowMaximizeButtonHint, true);
    resize(1180, 760);

    maxParallel_ = qBound(1, QSettings().value(QStringLiteral("transfer/parallel"), 2).toInt(), 4);
    editRoot_ = QDir::tempPath() + QStringLiteral("/sshmanager-edit-")
                + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);

    buildUi();
    startBrowser();
}

FileTransferDialog::~FileTransferDialog()
{
    stopSessions();
    QDir(editRoot_).removeRecursively();
    // The sessions share the plan's key and askpass files: delete them last.
    for (const QString &path : plan_.temporaryFiles)
        QFile::remove(path);
}

// ---------------------------------------------------------------------------
// Layout

void FileTransferDialog::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    auto *title = new QLabel(tr("<b>%1</b> — %2").arg(connection_.displayName().toHtmlEscaped(),
                                                       connection_.target().toHtmlEscaped()),
                             this);
    statusLabel_ = new QLabel(tr("Connessione in corso…"), this);
    statusLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statusLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    statusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *header = new QHBoxLayout;
    header->addWidget(title);
    header->addWidget(statusLabel_, 1);
    layout->addLayout(header);

    const auto toolButton = [this](QWidget *parent, const QString &theme, QStyle::StandardPixmap fallback,
                                   const QString &tip) {
        auto *button = new QToolButton(parent);
        button->setIcon(QIcon::fromTheme(theme, style()->standardIcon(fallback)));
        button->setToolTip(tip);
        button->setAutoRaise(true);
        return button;
    };

    auto *splitter = new QSplitter(Qt::Horizontal, this);

    // --- Local pane --------------------------------------------------------
    auto *localBox = new QGroupBox(tr("Locale"), splitter);
    auto *localLayout = new QVBoxLayout(localBox);
    auto *localBar = new QHBoxLayout;
    auto *localUp = toolButton(localBox, QStringLiteral("go-up"), QStyle::SP_ArrowUp,
                               tr("Cartella superiore"));
    localPathEdit_ = new QLineEdit(localBox);
    // A star reads as "bookmarks" everywhere; theme icons are often missing.
    const auto bookmarkButton = [](QWidget *parent, const QString &tip) {
        auto *button = new QToolButton(parent);
        button->setText(QStringLiteral("★"));
        button->setToolTip(tip);
        button->setAutoRaise(true);
        return button;
    };
    localBookmarks_ = bookmarkButton(localBox, tr("Segnalibri locali"));
    localBar->addWidget(localUp);
    localBar->addWidget(localPathEdit_, 1);
    localBar->addWidget(localBookmarks_);

    localModel_ = new QFileSystemModel(this);
    localModel_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    localModel_->setReadOnly(true); // changes go through explicit actions only
#ifdef _WIN32
    localModel_->setRootPath(QString()); // also lists every drive
#else
    localModel_->setRootPath(QDir::rootPath());
#endif

    localView_ = new QTreeView(localBox);
    localView_->setModel(localModel_);
    localView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    localView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    localView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    localView_->setUniformRowHeights(true);
    localView_->setRootIsDecorated(false);
    localView_->setItemsExpandable(false);
    localView_->setSortingEnabled(true);
    localView_->sortByColumn(0, Qt::AscendingOrder);
    localView_->hideColumn(2); // type
    localView_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    localView_->header()->setStretchLastSection(false);
    localView_->setContextMenuPolicy(Qt::CustomContextMenu);
    localView_->setDragEnabled(true);
    localView_->setDragDropMode(QAbstractItemView::DragDrop);
    localView_->setDefaultDropAction(Qt::CopyAction);
    localView_->viewport()->setAcceptDrops(true);
    localView_->viewport()->installEventFilter(this);
    localLayout->addLayout(localBar);
    localLayout->addWidget(localView_, 1);

    // --- Remote pane -------------------------------------------------------
    auto *remoteBox = new QGroupBox(tr("Remoto"), splitter);
    auto *remoteLayout = new QVBoxLayout(remoteBox);
    auto *remoteBar = new QHBoxLayout;
    auto *remoteUp = toolButton(remoteBox, QStringLiteral("go-up"), QStyle::SP_ArrowUp,
                                tr("Cartella superiore"));
    auto *remoteHome = toolButton(remoteBox, QStringLiteral("go-home"), QStyle::SP_DirHomeIcon,
                                  tr("Torna alla home remota"));
    auto *remoteRefresh = toolButton(remoteBox, QStringLiteral("view-refresh"),
                                     QStyle::SP_BrowserReload, tr("Aggiorna (F5)"));
    remotePathEdit_ = new QLineEdit(remoteBox);
    remoteBookmarks_ = bookmarkButton(remoteBox, tr("Segnalibri remoti"));
    remoteBar->addWidget(remoteUp);
    remoteBar->addWidget(remoteHome);
    remoteBar->addWidget(remotePathEdit_, 1);
    remoteBar->addWidget(remoteRefresh);
    remoteBar->addWidget(remoteBookmarks_);

    remoteModel_ = new RemoteModel(0, 4, this);
    remoteModel_->setHorizontalHeaderLabels(
        {tr("Nome"), tr("Dimensione"), tr("Modificato"), tr("Permessi")});
    remoteView_ = new QTreeView(remoteBox);
    remoteView_->setModel(remoteModel_);
    remoteView_->setRootIsDecorated(false);
    remoteView_->setUniformRowHeights(true);
    remoteView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    remoteView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    remoteView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    remoteView_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 4; ++column)
        remoteView_->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    remoteView_->setContextMenuPolicy(Qt::CustomContextMenu);
    remoteView_->setDragEnabled(true);
    remoteView_->setDragDropMode(QAbstractItemView::DragDrop);
    remoteView_->setDefaultDropAction(Qt::CopyAction);
    remoteView_->viewport()->setAcceptDrops(true);
    remoteView_->viewport()->installEventFilter(this);
    remoteLayout->addLayout(remoteBar);
    remoteLayout->addWidget(remoteView_, 1);

    // --- Buttons between the panes -----------------------------------------
    auto *actions = new QWidget(splitter);
    auto *actionsLayout = new QVBoxLayout(actions);
    actionsLayout->setContentsMargins(4, 0, 4, 0);
    actionsLayout->addStretch();
    uploadButton_ = new QPushButton(tr("Invia →"), actions);
    uploadButton_->setIcon(QIcon::fromTheme(QStringLiteral("go-next"),
                                            style()->standardIcon(QStyle::SP_ArrowRight)));
    uploadButton_->setToolTip(tr("Accoda l'invio degli elementi locali selezionati"));
    uploadButton_->setMinimumHeight(34);
    downloadButton_ = new QPushButton(tr("← Ricevi"), actions);
    downloadButton_->setIcon(QIcon::fromTheme(QStringLiteral("go-previous"),
                                              style()->standardIcon(QStyle::SP_ArrowLeft)));
    downloadButton_->setToolTip(tr("Accoda la ricezione degli elementi remoti selezionati"));
    downloadButton_->setMinimumHeight(34);
    actionsLayout->addWidget(uploadButton_);
    actionsLayout->addWidget(downloadButton_);
    auto *dragHint = new QLabel(tr("oppure\ntrascina"), actions);
    dragHint->setAlignment(Qt::AlignHCenter);
    dragHint->setEnabled(false);
    actionsLayout->addWidget(dragHint);
    actionsLayout->addStretch();

    splitter->addWidget(localBox);
    splitter->addWidget(actions);
    splitter->addWidget(remoteBox);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    splitter->setStretchFactor(2, 1);

    // --- Queue -------------------------------------------------------------
    auto *queueBox = new QGroupBox(tr("Coda di trasferimento"), this);
    auto *queueLayout = new QVBoxLayout(queueBox);
    auto *queueBar = new QHBoxLayout;
    summaryLabel_ = new QLabel(queueBox);
    queueBar->addWidget(summaryLabel_, 1);
    queueBar->addWidget(new QLabel(tr("Contemporanei:"), queueBox));
    parallel_ = new QSpinBox(queueBox);
    parallel_->setRange(1, 4);
    parallel_->setValue(maxParallel_);
    parallel_->setToolTip(tr("Trasferimenti eseguiti in parallelo, ognuno con la propria connessione"));
    queueBar->addWidget(parallel_);
    auto *retryFailed = new QPushButton(tr("Riprova non riusciti"), queueBox);
    auto *clearDone = new QPushButton(tr("Rimuovi completati"), queueBox);
    queueBar->addWidget(retryFailed);
    queueBar->addWidget(clearDone);
    queueLayout->addLayout(queueBar);

    queue_ = new QTableWidget(0, 5, queueBox);
    queue_->setHorizontalHeaderLabels(
        {tr("Elemento"), tr("Direzione"), tr("Dimensione"), tr("Avanzamento"), tr("Stato")});
    queue_->horizontalHeader()->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
    queue_->horizontalHeader()->setSectionResizeMode(DirectionColumn, QHeaderView::ResizeToContents);
    queue_->horizontalHeader()->setSectionResizeMode(SizeColumn, QHeaderView::ResizeToContents);
    queue_->horizontalHeader()->setSectionResizeMode(ProgressColumn, QHeaderView::Fixed);
    queue_->horizontalHeader()->resizeSection(ProgressColumn, 180);
    queue_->horizontalHeader()->setSectionResizeMode(StateColumn, QHeaderView::Interactive);
    queue_->horizontalHeader()->resizeSection(StateColumn, 220);
    queue_->verticalHeader()->setVisible(false);
    queue_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    queue_->setSelectionBehavior(QAbstractItemView::SelectRows);
    queue_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    queue_->setContextMenuPolicy(Qt::CustomContextMenu);
    queue_->setAlternatingRowColors(true);
    queueLayout->addWidget(queue_);

    auto *vertical = new QSplitter(Qt::Vertical, this);
    vertical->addWidget(splitter);
    vertical->addWidget(queueBox);
    vertical->setStretchFactor(0, 3);
    vertical->setStretchFactor(1, 1);
    layout->addWidget(vertical, 1);

    auto *bottom = new QHBoxLayout;
    bottom->addStretch();
    auto *closeButton = new QPushButton(tr("Chiudi"), this);
    bottom->addWidget(closeButton);
    layout->addLayout(bottom);

    // --- Keyboard shortcuts on the panes ------------------------------------
    const auto shortcut = [](QWidget *view, const QKeySequence &key, auto slot, QObject *context) {
        auto *action = new QAction(view);
        action->setShortcut(key);
        action->setShortcutContext(Qt::WidgetShortcut);
        view->addAction(action);
        QObject::connect(action, &QAction::triggered, context, slot);
    };
    shortcut(remoteView_, QKeySequence::Delete, [this] { remoteDelete(); }, this);
    shortcut(remoteView_, QKeySequence(Qt::Key_F2), [this] { remoteRename(); }, this);
    shortcut(remoteView_, QKeySequence(Qt::Key_F5), [this] { refreshRemote(); }, this);
    shortcut(localView_, QKeySequence::Delete, [this] { localDelete(); }, this);
    shortcut(localView_, QKeySequence(Qt::Key_F2), [this] { localRename(); }, this);

    // --- Connections ---------------------------------------------------------
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(uploadButton_, &QPushButton::clicked, this,
            [this] { enqueue(true, selectedLocalPaths(), remoteDir_); });
    connect(downloadButton_, &QPushButton::clicked, this,
            [this] { enqueue(false, selectedRemotePaths(), localDir_); });
    connect(parallel_, &QSpinBox::valueChanged, this, [this](int value) {
        maxParallel_ = value;
        QSettings().setValue(QStringLiteral("transfer/parallel"), value);
        dispatch();
    });
    connect(retryFailed, &QPushButton::clicked, this, [this] {
        QList<int> ids;
        for (const Job &j : jobs_) {
            if (j.state == Job::State::Failed || j.state == Job::State::Cancelled)
                ids << j.id;
        }
        retryJobs(ids);
    });
    connect(clearDone, &QPushButton::clicked, this, [this] {
        QList<int> ids;
        for (const Job &j : jobs_) {
            if (j.state == Job::State::Done)
                ids << j.id;
        }
        removeJobs(ids);
    });
    connect(queue_, &QTableWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        const QList<int> ids = selectedJobIds();
        if (ids.isEmpty())
            return;
        QMenu menu(this);
        QAction *cancel = menu.addAction(tr("Annulla"));
        QAction *retry = menu.addAction(tr("Riprova"));
        QAction *remove = menu.addAction(tr("Rimuovi dalla lista"));
        QAction *chosen = menu.exec(queue_->viewport()->mapToGlobal(pos));
        if (chosen == cancel)
            cancelJobs(ids);
        else if (chosen == retry)
            retryJobs(ids);
        else if (chosen == remove)
            removeJobs(ids);
    });

    connect(localView_, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) {
        if (!index.isValid())
            return;
        if (localModel_->isDir(index))
            navigateLocal(localModel_->filePath(index));
        else
            enqueue(true, {localModel_->filePath(index)}, remoteDir_);
    });
    connect(remoteView_, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) {
        if (!index.isValid())
            return;
        const QModelIndex first = index.sibling(index.row(), 0);
        const QString path = first.data(PathRole).toString();
        if (first.data(DirRole).toBool())
            navigateRemote(path);
        else
            enqueue(false, {path}, localDir_);
    });
    connect(localView_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { updateControls(); });
    connect(remoteView_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { updateControls(); });
    connect(localView_, &QTreeView::customContextMenuRequested, this,
            &FileTransferDialog::localContextMenu);
    connect(remoteView_, &QTreeView::customContextMenuRequested, this,
            &FileTransferDialog::remoteContextMenu);

#ifdef _WIN32
    connect(localUp, &QToolButton::clicked, this,
            [this] { navigateLocal(localParentPath(localDir_)); });
#else
    connect(localUp, &QToolButton::clicked, this, [this] { navigateLocal(parentPath(localDir_)); });
#endif
    connect(remoteUp, &QToolButton::clicked, this,
            [this] { navigateRemote(parentPath(remoteDir_)); });
    connect(remoteHome, &QToolButton::clicked, this, [this] {
        if (!remoteHome_.isEmpty())
            navigateRemote(remoteHome_);
    });
    connect(remoteRefresh, &QToolButton::clicked, this, &FileTransferDialog::refreshRemote);
    connect(localPathEdit_, &QLineEdit::returnPressed, this,
            [this] { navigateLocal(localPathEdit_->text()); });
    connect(remotePathEdit_, &QLineEdit::returnPressed, this,
            [this] { navigateRemote(remotePathEdit_->text()); });
    connect(localBookmarks_, &QToolButton::clicked, this, [this] { showBookmarks(false); });
    connect(remoteBookmarks_, &QToolButton::clicked, this, [this] { showBookmarks(true); });

    editWatcher_ = new QFileSystemWatcher(this);
    connect(editWatcher_, &QFileSystemWatcher::fileChanged, this,
            &FileTransferDialog::onEditedFileTouched);
    connect(editWatcher_, &QFileSystemWatcher::directoryChanged, this,
            &FileTransferDialog::onEditedFileTouched);

    navigateLocal(QDir::homePath());
    updateSummary();
    updateControls();
}

// ---------------------------------------------------------------------------
// Sessions

void FileTransferDialog::startBrowser()
{
    if (!plan_.valid) {
        setStatus(plan_.error);
        QMessageBox::critical(this, tr("Trasferimento file"), plan_.error);
        return;
    }

    browserThread_ = new QThread(this);
    browser_ = new SftpSession;
    browser_->configure(plan_);
    browser_->setOwnsFiles(false);
    browser_->moveToThread(browserThread_);
    connect(browser_, &SftpSession::opened, this, &FileTransferDialog::onOpened);
    connect(browser_, &SftpSession::listed, this, &FileTransferDialog::onListed);
    connect(browser_, &SftpSession::failed, this, &FileTransferDialog::onFailed);
    connect(browser_, &SftpSession::operationFinished, this,
            &FileTransferDialog::onOperationFinished);
    connect(browser_, &SftpSession::editFetched, this, &FileTransferDialog::onEditFetched);
    connect(browser_, &SftpSession::editStored, this, &FileTransferDialog::onEditStored);
    browserThread_->start();
    QMetaObject::invokeMethod(browser_, "open", Qt::QueuedConnection);
}

FileTransferDialog::Worker *FileTransferDialog::createWorker()
{
    auto *worker = new Worker;
    worker->thread = new QThread(this);
    worker->session = new SftpSession;
    worker->session->configure(plan_);
    worker->session->setOwnsFiles(false);
    worker->session->moveToThread(worker->thread);
    connect(worker->session, &SftpSession::jobPlanned, this, &FileTransferDialog::onJobPlanned);
    connect(worker->session, &SftpSession::jobProgress, this, &FileTransferDialog::onJobProgress);
    connect(worker->session, &SftpSession::jobFinished, this, &FileTransferDialog::onJobFinished);
    worker->thread->start();
    workers_.append(worker);
    return worker;
}

void FileTransferDialog::stopSessions()
{
    QVector<QPair<QThread *, SftpSession *>> all;
    if (browser_)
        all.append({browserThread_, browser_});
    for (Worker *worker : workers_)
        all.append({worker->thread, worker->session});
    // Cancel everything first, so that no session keeps the others waiting.
    for (const auto &pair : all)
        pair.second->requestCancel();
    for (const auto &pair : all) {
        QMetaObject::invokeMethod(pair.second, "close", Qt::BlockingQueuedConnection);
        pair.first->quit();
        pair.first->wait(5000);
        delete pair.second; // the thread has stopped: safe from here
    }
    qDeleteAll(workers_);
    workers_.clear();
    browser_ = nullptr;
}

// ---------------------------------------------------------------------------
// Navigation

void FileTransferDialog::navigateLocal(const QString &path)
{
#ifdef _WIN32
    if (path.trimmed().isEmpty()) {
        // drive list: nothing can be downloaded here
        localDir_.clear();
        localView_->setRootIndex(QModelIndex());
        localPathEdit_->clear();
        updateControls();
        return;
    }
#endif
    QString clean = QDir::cleanPath(path.trimmed());
    QFileInfo info(clean);
    if (!info.isDir())
        info = QFileInfo(info.absolutePath());
    // The canonical form also expands Windows short names ("FABRIZ~1"), which
    // the file system model would not find.
    clean = info.canonicalFilePath();
    const QModelIndex root = clean.isEmpty() ? QModelIndex() : localModel_->index(clean);
    if (!root.isValid()) {
        // Keep the pane and the current folder consistent: refuse the path.
        localPathEdit_->setText(QDir::toNativeSeparators(localDir_));
        setStatus(tr("Cartella locale non trovata: %1").arg(QDir::toNativeSeparators(path.trimmed())));
        return;
    }
    localDir_ = clean;
    localView_->setRootIndex(root);
    localPathEdit_->setText(QDir::toNativeSeparators(clean));
    updateControls();
}

void FileTransferDialog::navigateRemote(const QString &path)
{
    if (!browser_ || path.trimmed().isEmpty())
        return;
    remoteDir_ = path.trimmed();
    remotePathEdit_->setText(remoteDir_);
    setStatus(tr("Lettura di %1…").arg(remoteDir_));
    QMetaObject::invokeMethod(browser_, "listDirectory", Qt::QueuedConnection,
                              Q_ARG(QString, remoteDir_));
}

void FileTransferDialog::refreshRemote()
{
    if (connected_ && !remoteDir_.isEmpty())
        navigateRemote(remoteDir_);
}

void FileTransferDialog::onOpened(const QString &home)
{
    connected_ = true;
    remoteHome_ = home;
    setStatus(tr("Connesso a %1").arg(connection_.target()));
    navigateRemote(home);
    updateControls();
}

void FileTransferDialog::onListed(const QString &path, const QVector<RemoteEntry> &entries)
{
    if (path != remoteDir_)
        return;
    connected_ = true;
    remoteEntries_ = entries;
    remoteModel_->removeRows(0, remoteModel_->rowCount());
    for (const RemoteEntry &entry : entries) {
        auto *nameItem = new QStandardItem(style()->standardIcon(
                                               entry.isLink ? QStyle::SP_FileLinkIcon
                                                            : (entry.isDir ? QStyle::SP_DirIcon
                                                                           : QStyle::SP_FileIcon)),
                                           entry.name);
        nameItem->setData(entry.path, PathRole);
        nameItem->setData(entry.isDir, DirRole);
        nameItem->setData(entry.mode, ModeRole);
        nameItem->setData(entry.size, SizeRole);
        nameItem->setToolTip(entry.path);
        auto *sizeItem = new QStandardItem(entry.isDir ? QStringLiteral("—") : formatSize(entry.size));
        sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto *dateItem = new QStandardItem(
            entry.modified.isValid() ? QLocale().toString(entry.modified, QLocale::ShortFormat)
                                     : QStringLiteral("—"));
        auto *modeItem = new QStandardItem(permissionText(entry.mode, entry.isDir, entry.isLink));
        for (QStandardItem *item : {nameItem, sizeItem, dateItem, modeItem})
            item->setEditable(false);
        remoteModel_->appendRow({nameItem, sizeItem, dateItem, modeItem});
    }
    setStatus(tr("%1 elementi in %2").arg(entries.size()).arg(path));
    updateControls();
}

void FileTransferDialog::onFailed(const QString &error)
{
    setStatus(error.section(QLatin1Char('\n'), 0, 0));
    if (!connected_) {
        QMessageBox::critical(this, tr("Trasferimento file"), error);
        updateControls();
        return;
    }
    QMessageBox::warning(this, tr("Cartella remota"), error);
}

// ---------------------------------------------------------------------------
// Queue

void FileTransferDialog::enqueue(bool upload, const QStringList &sources,
                                 const QString &destinationDir)
{
    if (!connected_ || sources.isEmpty() || destinationDir.isEmpty())
        return;
    for (const QString &source : sources) {
        Job job;
        job.id = nextJobId_++;
        job.upload = upload;
        job.source = source;
        job.destinationDir = destinationDir;
        if (upload) {
            const QFileInfo info(source);
            job.name = info.fileName();
            job.directory = info.isDir();
            if (!job.directory)
                job.total = info.size();
        } else {
            job.name = baseName(source);
            for (const RemoteEntry &entry : remoteEntries_) {
                if (entry.path == source) {
                    job.directory = entry.isDir;
                    if (!entry.isDir)
                        job.total = entry.size;
                    break;
                }
            }
        }
        jobs_.append(job);
        updateJobRow(job);
    }
    dispatch();
    updateSummary();
}

void FileTransferDialog::dispatch()
{
    for (Job &candidate : jobs_) {
        if (candidate.state != Job::State::Queued)
            continue;
        int busy = 0;
        Worker *free = nullptr;
        for (Worker *worker : workers_) {
            if (worker->jobId >= 0)
                ++busy;
            else if (!free)
                free = worker;
        }
        if (busy >= maxParallel_)
            break;
        if (!free)
            free = createWorker();

        free->jobId = candidate.id;
        candidate.state = Job::State::Running;
        candidate.done = 0;
        candidate.current.clear();
        updateJobRow(candidate);
        QMetaObject::invokeMethod(free->session, "runJob", Qt::QueuedConnection,
                                  Q_ARG(int, candidate.id), Q_ARG(bool, candidate.upload),
                                  Q_ARG(QString, candidate.source),
                                  Q_ARG(QString, candidate.destinationDir),
                                  Q_ARG(bool, candidate.resume));
    }
    updateSummary();
}

void FileTransferDialog::onJobPlanned(int jobId, qint64 total, int files)
{
    if (Job *j = job(jobId)) {
        j->total = total;
        j->files = files;
        updateJobRow(*j);
    }
}

void FileTransferDialog::onJobProgress(int jobId, qint64 done, qint64 total, const QString &current)
{
    if (Job *j = job(jobId)) {
        j->done = done;
        j->total = total;
        if (!current.isEmpty())
            j->current = current;
        updateJobRow(*j);
    }
}

void FileTransferDialog::onJobFinished(int jobId, bool ok, const QString &error, bool cancelled,
                                       bool connectionLost)
{
    for (Worker *worker : workers_) {
        if (worker->jobId == jobId)
            worker->jobId = -1;
    }
    Job *j = job(jobId);
    if (!j) {
        dispatch();
        return;
    }
    if (ok) {
        j->state = Job::State::Done;
        j->done = qMax<qint64>(0, j->total);
        j->error.clear();
        // Show new remote files straight away.
        if (j->upload && j->destinationDir == remoteDir_)
            refreshRemote();
    } else if (cancelled) {
        j->state = Job::State::Cancelled;
        j->error = error;
    } else if (connectionLost && j->attempts < kMaxAttempts) {
        // A dropped connection: try again, continuing where it stopped.
        ++j->attempts;
        j->state = Job::State::Queued;
        j->resume = true;
        j->error = tr("Connessione persa, nuovo tentativo %1 di %2…").arg(j->attempts).arg(kMaxAttempts);
        QTimer::singleShot(3000, this, &FileTransferDialog::dispatch);
        updateJobRow(*j);
        updateSummary();
        return;
    } else {
        j->state = Job::State::Failed;
        j->resume = true; // a manual retry continues the partial files
        j->error = error;
    }
    updateJobRow(*j);
    dispatch();
}

void FileTransferDialog::cancelJobs(const QList<int> &ids)
{
    for (int id : ids) {
        Job *j = job(id);
        if (!j)
            continue;
        if (j->state == Job::State::Queued) {
            j->state = Job::State::Cancelled;
            j->error = tr("Trasferimento annullato.");
            updateJobRow(*j);
        } else if (j->state == Job::State::Running) {
            for (Worker *worker : workers_) {
                if (worker->jobId == id)
                    worker->session->requestCancel();
            }
        }
    }
    updateSummary();
}

void FileTransferDialog::retryJobs(const QList<int> &ids)
{
    for (int id : ids) {
        Job *j = job(id);
        if (!j || (j->state != Job::State::Failed && j->state != Job::State::Cancelled))
            continue;
        j->state = Job::State::Queued;
        j->attempts = 0;
        j->resume = true;
        j->error.clear();
        updateJobRow(*j);
    }
    dispatch();
}

void FileTransferDialog::removeJobs(const QList<int> &ids)
{
    for (int id : ids) {
        Job *j = job(id);
        if (!j || j->state == Job::State::Running)
            continue;
        if (j->state == Job::State::Queued)
            j->state = Job::State::Cancelled;
        // A failed single-file download leaves its partial copy behind.
        if (!j->upload && !j->directory && j->state != Job::State::Done)
            QFile::remove(QDir(j->destinationDir).filePath(j->name) + partialSuffix());
        const int row = rowOfJob(id);
        if (row >= 0)
            queue_->removeRow(row);
        for (int i = 0; i < jobs_.size(); ++i) {
            if (jobs_.at(i).id == id) {
                jobs_.removeAt(i);
                break;
            }
        }
    }
    updateSummary();
}

int FileTransferDialog::rowOfJob(int id) const
{
    for (int row = 0; row < queue_->rowCount(); ++row) {
        if (const QTableWidgetItem *item = queue_->item(row, NameColumn);
            item && item->data(Qt::UserRole).toInt() == id)
            return row;
    }
    return -1;
}

FileTransferDialog::Job *FileTransferDialog::job(int id)
{
    for (Job &j : jobs_) {
        if (j.id == id)
            return &j;
    }
    return nullptr;
}

QList<int> FileTransferDialog::selectedJobIds() const
{
    QList<int> ids;
    for (const QModelIndex &index : queue_->selectionModel()->selectedRows()) {
        if (const QTableWidgetItem *item = queue_->item(index.row(), NameColumn))
            ids << item->data(Qt::UserRole).toInt();
    }
    return ids;
}

bool FileTransferDialog::hasActiveJobs() const
{
    for (const Job &j : jobs_) {
        if (j.state == Job::State::Queued || j.state == Job::State::Running)
            return true;
    }
    return false;
}

void FileTransferDialog::updateJobRow(const Job &job)
{
    int row = rowOfJob(job.id);
    if (row < 0) {
        row = queue_->rowCount();
        queue_->insertRow(row);
        auto *name = new QTableWidgetItem(
            style()->standardIcon(job.directory ? QStyle::SP_DirIcon : QStyle::SP_FileIcon), job.name);
        name->setData(Qt::UserRole, job.id);
        name->setToolTip(tr("%1\n→ %2").arg(job.source, job.destinationDir));
        queue_->setItem(row, NameColumn, name);
        queue_->setItem(row, DirectionColumn,
                        new QTableWidgetItem(job.upload ? tr("Invio") : tr("Ricezione")));
        auto *size = new QTableWidgetItem;
        size->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        queue_->setItem(row, SizeColumn, size);
        auto *bar = new QProgressBar(queue_);
        bar->setRange(0, kProgressSteps);
        bar->setTextVisible(true);
        bar->setMaximumHeight(18);
        queue_->setCellWidget(row, ProgressColumn, bar);
        queue_->setItem(row, StateColumn, new QTableWidgetItem);
    }

    QString sizeText = job.total >= 0 ? formatSize(job.total)
                                      : (job.directory ? tr("cartella") : QStringLiteral("—"));
    if (job.files > 1)
        sizeText = tr("%1 file · %2").arg(job.files).arg(sizeText);
    queue_->item(row, SizeColumn)->setText(sizeText);

    if (auto *bar = qobject_cast<QProgressBar *>(queue_->cellWidget(row, ProgressColumn))) {
        int value = 0;
        if (job.state == Job::State::Done)
            value = kProgressSteps;
        else if (job.total > 0)
            value = int(qBound(qint64(0), job.done * kProgressSteps / job.total,
                               qint64(kProgressSteps)));
        bar->setValue(value);
        bar->setFormat(job.total > 0 && job.state == Job::State::Running
                           ? tr("%p% · %1").arg(formatSize(job.done))
                           : QStringLiteral("%p%"));
    }

    QTableWidgetItem *state = queue_->item(row, StateColumn);
    QString text;
    QColor color = palette().color(QPalette::Text);
    switch (job.state) {
    case Job::State::Queued:
        text = job.error.isEmpty() ? tr("In coda") : job.error;
        break;
    case Job::State::Running:
        text = job.current.isEmpty() ? tr("In corso") : tr("In corso — %1").arg(job.current);
        break;
    case Job::State::Done:
        text = tr("Completato");
        color = apptheme::success();
        break;
    case Job::State::Failed:
        text = tr("Errore: %1").arg(job.error.section(QLatin1Char('\n'), 0, 0));
        color = apptheme::danger();
        break;
    case Job::State::Cancelled:
        text = tr("Annullato");
        color = apptheme::muted();
        break;
    }
    state->setText(text);
    state->setToolTip(job.error.isEmpty() ? text : job.error);
    state->setForeground(color);
}

void FileTransferDialog::updateSummary()
{
    int queued = 0;
    int running = 0;
    int done = 0;
    int failed = 0;
    for (const Job &j : jobs_) {
        switch (j.state) {
        case Job::State::Queued:
            ++queued;
            break;
        case Job::State::Running:
            ++running;
            break;
        case Job::State::Done:
            ++done;
            break;
        case Job::State::Failed:
            ++failed;
            break;
        case Job::State::Cancelled:
            break;
        }
    }
    summaryLabel_->setText(jobs_.isEmpty()
                               ? tr("Nessun trasferimento. Trascina file tra i pannelli o usa "
                                    "Invia/Ricevi.")
                               : tr("In corso: %1 · in coda: %2 · completati: %3 · errori: %4")
                                     .arg(running)
                                     .arg(queued)
                                     .arg(done)
                                     .arg(failed));
}

// ---------------------------------------------------------------------------
// Remote operations

void FileTransferDialog::remoteContextMenu(const QPoint &pos)
{
    if (!connected_)
        return;
    const QVector<RemoteEntry> entries = selectedRemoteEntries();
    const bool single = entries.size() == 1;
    const bool singleFile = single && !entries.first().isDir;

    QMenu menu(this);
    QAction *download = menu.addAction(tr("Ricevi in %1").arg(QDir::toNativeSeparators(localDir_)));
    download->setEnabled(!entries.isEmpty() && !localDir_.isEmpty());
    QAction *edit = menu.addAction(tr("Modifica con editor locale"));
    edit->setEnabled(singleFile);
    menu.addSeparator();
    QAction *newFolder = menu.addAction(tr("Nuova cartella…"));
    QAction *rename = menu.addAction(tr("Rinomina…"));
    rename->setShortcut(QKeySequence(Qt::Key_F2));
    rename->setEnabled(single);
    QAction *permissions = menu.addAction(tr("Permessi…"));
    permissions->setEnabled(!entries.isEmpty());
    QAction *remove = menu.addAction(tr("Elimina"));
    remove->setShortcut(QKeySequence::Delete);
    remove->setEnabled(!entries.isEmpty());
    menu.addSeparator();
    QAction *copyPath = menu.addAction(tr("Copia percorso"));
    copyPath->setEnabled(!entries.isEmpty());
    QAction *refresh = menu.addAction(tr("Aggiorna"));
    refresh->setShortcut(QKeySequence(Qt::Key_F5));

    QAction *chosen = menu.exec(remoteView_->viewport()->mapToGlobal(pos));
    if (chosen == download)
        enqueue(false, selectedRemotePaths(), localDir_);
    else if (chosen == edit)
        remoteEdit();
    else if (chosen == newFolder)
        remoteNewFolder();
    else if (chosen == rename)
        remoteRename();
    else if (chosen == permissions)
        remotePermissions();
    else if (chosen == remove)
        remoteDelete();
    else if (chosen == copyPath)
        QApplication::clipboard()->setText(selectedRemotePaths().join(QLatin1Char('\n')));
    else if (chosen == refresh)
        refreshRemote();
}

void FileTransferDialog::remoteNewFolder()
{
    if (!connected_ || remoteDir_.isEmpty())
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Nuova cartella remota"),
                                               tr("Nome della cartella in %1:").arg(remoteDir_),
                                               QLineEdit::Normal, QString(), &ok)
                             .trimmed();
    if (!ok || name.isEmpty())
        return;
    if (name.contains(QLatin1Char('/'))) {
        QMessageBox::warning(this, tr("Nuova cartella remota"), tr("Il nome non può contenere \"/\"."));
        return;
    }
    QMetaObject::invokeMethod(browser_, "makeDirectory", Qt::QueuedConnection,
                              Q_ARG(QString, joinRemote(remoteDir_, name)));
}

void FileTransferDialog::remoteRename()
{
    const QVector<RemoteEntry> entries = selectedRemoteEntries();
    if (!connected_ || entries.size() != 1)
        return;
    const RemoteEntry entry = entries.first();
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rinomina"), tr("Nuovo nome:"),
                                               QLineEdit::Normal, entry.name, &ok)
                             .trimmed();
    if (!ok || name.isEmpty() || name == entry.name)
        return;
    if (name.contains(QLatin1Char('/'))) {
        QMessageBox::warning(this, tr("Rinomina"), tr("Il nome non può contenere \"/\"."));
        return;
    }
    QMetaObject::invokeMethod(browser_, "renamePath", Qt::QueuedConnection,
                              Q_ARG(QString, entry.path),
                              Q_ARG(QString, joinRemote(parentPath(entry.path), name)));
}

void FileTransferDialog::remoteDelete()
{
    const QStringList paths = selectedRemotePaths();
    if (!connected_ || paths.isEmpty())
        return;
    const QString what = paths.size() == 1 ? baseName(paths.first())
                                           : tr("%1 elementi").arg(paths.size());
    if (QMessageBox::warning(this, tr("Elimina dal server"),
                             tr("Eliminare definitivamente %1 dal server? Le cartelle vengono "
                                "eliminate con tutto il contenuto.")
                                 .arg(what),
                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes)
        return;
    setStatus(tr("Eliminazione in corso…"));
    QMetaObject::invokeMethod(browser_, "removePaths", Qt::QueuedConnection,
                              Q_ARG(QStringList, paths));
}

void FileTransferDialog::remotePermissions()
{
    const QVector<RemoteEntry> entries = selectedRemoteEntries();
    if (!connected_ || entries.isEmpty())
        return;
    const QString what = entries.size() == 1 ? entries.first().path
                                             : tr("%1 elementi selezionati").arg(entries.size());
    quint32 mode = 0;
    if (!askPermissions(this, what, entries.first().mode & 07777, &mode))
        return;
    QStringList paths;
    for (const RemoteEntry &entry : entries)
        paths << entry.path;
    QMetaObject::invokeMethod(browser_, "setPermissions", Qt::QueuedConnection,
                              Q_ARG(QStringList, paths), Q_ARG(quint32, mode));
}

void FileTransferDialog::onOperationFinished(const QString &refreshDir, bool ok,
                                             const QString &error)
{
    if (!ok) {
        setStatus(error.section(QLatin1Char('\n'), 0, 0));
        QMessageBox::warning(this, tr("Operazione non riuscita"), error);
    }
    if (refreshDir.isEmpty() || refreshDir == remoteDir_ || !ok)
        refreshRemote();
}

// ---------------------------------------------------------------------------
// Local operations

void FileTransferDialog::localContextMenu(const QPoint &pos)
{
    const QStringList paths = selectedLocalPaths();
    QMenu menu(this);
    QAction *upload = menu.addAction(tr("Invia in %1").arg(remoteDir_));
    upload->setEnabled(connected_ && !paths.isEmpty() && !remoteDir_.isEmpty());
    QAction *open = menu.addAction(tr("Apri"));
    open->setEnabled(paths.size() == 1);
    menu.addSeparator();
    QAction *newFolder = menu.addAction(tr("Nuova cartella…"));
    newFolder->setEnabled(!localDir_.isEmpty());
    QAction *rename = menu.addAction(tr("Rinomina…"));
    rename->setShortcut(QKeySequence(Qt::Key_F2));
    rename->setEnabled(paths.size() == 1);
    QAction *remove = menu.addAction(tr("Sposta nel cestino"));
    remove->setShortcut(QKeySequence::Delete);
    remove->setEnabled(!paths.isEmpty());

    QAction *chosen = menu.exec(localView_->viewport()->mapToGlobal(pos));
    if (chosen == upload)
        enqueue(true, paths, remoteDir_);
    else if (chosen == open)
        QDesktopServices::openUrl(QUrl::fromLocalFile(paths.first()));
    else if (chosen == newFolder)
        localNewFolder();
    else if (chosen == rename)
        localRename();
    else if (chosen == remove)
        localDelete();
}

void FileTransferDialog::localNewFolder()
{
    if (localDir_.isEmpty())
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Nuova cartella locale"),
                                               tr("Nome della cartella in %1:")
                                                   .arg(QDir::toNativeSeparators(localDir_)),
                                               QLineEdit::Normal, QString(), &ok)
                             .trimmed();
    if (!ok || name.isEmpty())
        return;
    if (!QDir(localDir_).mkdir(name))
        QMessageBox::warning(this, tr("Nuova cartella locale"),
                             tr("Impossibile creare la cartella %1.").arg(name));
}

void FileTransferDialog::localRename()
{
    const QStringList paths = selectedLocalPaths();
    if (paths.size() != 1)
        return;
    const QFileInfo info(paths.first());
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rinomina"), tr("Nuovo nome:"),
                                               QLineEdit::Normal, info.fileName(), &ok)
                             .trimmed();
    if (!ok || name.isEmpty() || name == info.fileName())
        return;
    if (!QDir(info.absolutePath()).rename(info.fileName(), name))
        QMessageBox::warning(this, tr("Rinomina"),
                             tr("Impossibile rinominare %1.").arg(info.fileName()));
}

void FileTransferDialog::localDelete()
{
    const QStringList paths = selectedLocalPaths();
    if (paths.isEmpty())
        return;
    const QString what = paths.size() == 1 ? QFileInfo(paths.first()).fileName()
                                           : tr("%1 elementi").arg(paths.size());
    if (QMessageBox::question(this, tr("Sposta nel cestino"),
                              tr("Spostare %1 nel cestino?").arg(what))
        != QMessageBox::Yes)
        return;
    QStringList failed;
    for (const QString &path : paths) {
        if (!QFile::moveToTrash(path))
            failed << QDir::toNativeSeparators(path);
    }
    if (!failed.isEmpty())
        QMessageBox::warning(this, tr("Sposta nel cestino"),
                             tr("Impossibile spostare nel cestino:\n%1").arg(failed.join(QLatin1Char('\n'))));
}

// ---------------------------------------------------------------------------
// Editing remote files with a local program

void FileTransferDialog::remoteEdit()
{
    const QVector<RemoteEntry> entries = selectedRemoteEntries();
    if (!connected_ || entries.size() != 1 || entries.first().isDir)
        return;
    const RemoteEntry entry = entries.first();
    for (const EditedFile &file : std::as_const(edited_)) {
        if (file.remotePath == entry.path) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(file.localPath));
            return;
        }
    }
    if (entry.size > kEditWarnSize
        && QMessageBox::question(this, tr("Modifica file"),
                                 tr("%1 è grande %2. Scaricarlo per modificarlo?")
                                     .arg(entry.name, formatSize(entry.size)))
               != QMessageBox::Yes)
        return;

    const QString dir = editRoot_ + QLatin1Char('/')
                        + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    QDir().mkpath(dir);
    setStatus(tr("Download di %1 per la modifica…").arg(entry.name));
    QMetaObject::invokeMethod(browser_, "fetchForEdit", Qt::QueuedConnection,
                              Q_ARG(QString, entry.path), Q_ARG(QString, dir + QLatin1Char('/') + entry.name));
}

void FileTransferDialog::onEditFetched(const QString &remotePath, const QString &localPath, bool ok,
                                       const QString &error)
{
    if (!ok) {
        setStatus(error.section(QLatin1Char('\n'), 0, 0));
        QMessageBox::warning(this, tr("Modifica file"),
                             tr("Impossibile scaricare %1:\n%2").arg(remotePath, error));
        return;
    }
    EditedFile file;
    file.remotePath = remotePath;
    file.localPath = localPath;
    const QFileInfo info(localPath);
    file.uploadedStamp = info.lastModified();
    file.uploadedSize = info.size();
    edited_.insert(localPath, file);
    // Editors often save by writing a new file and renaming it: watch the
    // folder too, to notice the replacement.
    editWatcher_->addPath(localPath);
    editWatcher_->addPath(info.absolutePath());

    bool opened = QDesktopServices::openUrl(QUrl::fromLocalFile(localPath));
#ifdef _WIN32
    if (!opened)
        opened = QProcess::startDetached(QStringLiteral("notepad.exe"),
                                         {QDir::toNativeSeparators(localPath)});
#endif
    if (!opened) {
        QMessageBox::warning(this, tr("Modifica file"),
                             tr("Nessun programma associato a %1. Il file è in %2.")
                                 .arg(info.fileName(), QDir::toNativeSeparators(localPath)));
    }
    setStatus(tr("Modifica di %1: ogni salvataggio viene inviato al server").arg(info.fileName()));
}

void FileTransferDialog::onEditedFileTouched(const QString &path)
{
    QStringList files;
    if (edited_.contains(path)) {
        files << path;
    } else {
        // A watched folder changed: its edited files may have been replaced.
        for (auto it = edited_.cbegin(); it != edited_.cend(); ++it) {
            if (QFileInfo(it.key()).absolutePath() == QFileInfo(path).absoluteFilePath())
                files << it.key();
        }
    }
    for (const QString &file : files) {
        if (QFileInfo::exists(file) && !editWatcher_->files().contains(file))
            editWatcher_->addPath(file);
        // Wait until the editor has finished writing.
        QTimer *&timer = editTimers_[file];
        if (!timer) {
            timer = new QTimer(this);
            timer->setSingleShot(true);
            timer->setInterval(800);
            connect(timer, &QTimer::timeout, this, [this, file] { uploadEditedFile(file); });
        }
        timer->start();
    }
}

void FileTransferDialog::uploadEditedFile(const QString &localPath)
{
    auto it = edited_.find(localPath);
    if (it == edited_.end() || !browser_)
        return;
    const QFileInfo info(localPath);
    if (!info.exists())
        return;
    if (info.lastModified() == it->uploadedStamp && info.size() == it->uploadedSize)
        return; // touched but not changed
    if (it->uploading) {
        it->again = true;
        return;
    }
    it->uploading = true;
    it->uploadedStamp = info.lastModified();
    it->uploadedSize = info.size();
    setStatus(tr("Invio di %1 al server…").arg(info.fileName()));
    QMetaObject::invokeMethod(browser_, "storeFromEdit", Qt::QueuedConnection,
                              Q_ARG(QString, localPath), Q_ARG(QString, it->remotePath));
}

void FileTransferDialog::onEditStored(const QString &remotePath, bool ok, const QString &error)
{
    for (auto it = edited_.begin(); it != edited_.end(); ++it) {
        if (it->remotePath != remotePath)
            continue;
        it->uploading = false;
        const QString name = QFileInfo(it.key()).fileName();
        if (ok) {
            setStatus(tr("%1 salvato sul server alle %2")
                          .arg(name, QLocale().toString(QTime::currentTime(), QLocale::ShortFormat)));
            if (parentPath(remotePath) == remoteDir_)
                refreshRemote();
        } else {
            it->uploadedSize = -1; // send it again at the next save
            setStatus(error.section(QLatin1Char('\n'), 0, 0));
            QMessageBox::warning(this, tr("Modifica file"),
                                 tr("Impossibile salvare %1 sul server:\n%2").arg(name, error));
        }
        if (it->again) {
            it->again = false;
            uploadEditedFile(it.key());
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// Bookmarks

void FileTransferDialog::showBookmarks(bool remote)
{
    QSettings settings;
    QStringList bookmarks = remote
                                ? connection_.remoteBookmarks
                                : settings.value(QStringLiteral("transfer/localBookmarks")).toStringList();
    const QString current = remote ? remoteDir_ : localDir_;
    const QString shown = remote ? current : QDir::toNativeSeparators(current);

    QMenu menu(this);
    QAction *add = nullptr;
    QAction *removeCurrent = nullptr;
    if (!current.isEmpty()) {
        if (bookmarks.contains(current))
            removeCurrent = menu.addAction(tr("Rimuovi %1 dai segnalibri").arg(shown));
        else
            add = menu.addAction(tr("Aggiungi %1 ai segnalibri").arg(shown));
    }
    if (!bookmarks.isEmpty())
        menu.addSeparator();
    QHash<QAction *, QString> targets;
    for (const QString &bookmark : bookmarks) {
        QAction *action = menu.addAction(style()->standardIcon(QStyle::SP_DirIcon),
                                         remote ? bookmark : QDir::toNativeSeparators(bookmark));
        targets.insert(action, bookmark);
    }
    if (bookmarks.isEmpty() && !add)
        menu.addAction(tr("Nessun segnalibro"))->setEnabled(false);

    QToolButton *button = remote ? remoteBookmarks_ : localBookmarks_;
    QAction *chosen = menu.exec(button->mapToGlobal(QPoint(0, button->height())));
    if (!chosen)
        return;
    if (chosen == add || chosen == removeCurrent) {
        if (chosen == add)
            bookmarks << current;
        else
            bookmarks.removeAll(current);
        if (remote) {
            connection_.remoteBookmarks = bookmarks;
            emit remoteBookmarksChanged(connection_.id, bookmarks);
        } else {
            settings.setValue(QStringLiteral("transfer/localBookmarks"), bookmarks);
        }
        return;
    }
    if (targets.contains(chosen)) {
        if (remote)
            navigateRemote(targets.value(chosen));
        else
            navigateLocal(targets.value(chosen));
    }
}

// ---------------------------------------------------------------------------
// Drag and drop between the panes (and from the file manager)

bool FileTransferDialog::eventFilter(QObject *watched, QEvent *event)
{
    const bool remotePane = remoteView_ && watched == remoteView_->viewport();
    const bool localPane = localView_ && watched == localView_->viewport();
    if (!remotePane && !localPane)
        return QDialog::eventFilter(watched, event);

    const auto accepts = [&](const QMimeData *data) {
        if (!data || !connected_)
            return false;
        if (remotePane) {
            // Local files (our left pane or the system file manager).
            if (!data->hasUrls())
                return false;
            for (const QUrl &url : data->urls()) {
                if (!url.isLocalFile())
                    return false;
            }
            return true;
        }
        return data->hasFormat(QString::fromLatin1(kRemoteMime));
    };

    switch (event->type()) {
    case QEvent::DragEnter:
    case QEvent::DragMove: {
        auto *drag = static_cast<QDragMoveEvent *>(event);
        const QString target = remotePane ? remoteDropTarget(drag->position().toPoint())
                                          : localDropTarget(drag->position().toPoint());
        if (accepts(drag->mimeData()) && !target.isEmpty()) {
            drag->setDropAction(Qt::CopyAction); // never move: the source stays
            drag->accept();
        } else {
            drag->ignore();
        }
        return true;
    }
    case QEvent::Drop: {
        auto *drop = static_cast<QDropEvent *>(event);
        if (!accepts(drop->mimeData())) {
            drop->ignore();
            return true;
        }
        const QPoint pos = drop->position().toPoint();
        if (remotePane) {
            QStringList sources;
            for (const QUrl &url : drop->mimeData()->urls())
                sources << QDir::cleanPath(url.toLocalFile());
            enqueue(true, sources, remoteDropTarget(pos));
        } else {
            const QStringList sources =
                QString::fromUtf8(drop->mimeData()->data(QString::fromLatin1(kRemoteMime)))
                    .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            enqueue(false, sources, localDropTarget(pos));
        }
        drop->setDropAction(Qt::CopyAction);
        drop->accept();
        return true;
    }
    default:
        return QDialog::eventFilter(watched, event);
    }
}

QString FileTransferDialog::remoteDropTarget(const QPoint &viewportPos) const
{
    const QModelIndex index = remoteView_->indexAt(viewportPos);
    if (index.isValid()) {
        const QModelIndex first = index.sibling(index.row(), 0);
        if (first.data(DirRole).toBool())
            return first.data(PathRole).toString();
    }
    return remoteDir_;
}

QString FileTransferDialog::localDropTarget(const QPoint &viewportPos) const
{
    const QModelIndex index = localView_->indexAt(viewportPos);
    if (index.isValid() && localModel_->isDir(index))
        return localModel_->filePath(index);
    return localDir_;
}

// ---------------------------------------------------------------------------

void FileTransferDialog::reject()
{
    if (hasActiveJobs()
        && QMessageBox::question(this, tr("Trasferimento file"),
                                 tr("Ci sono trasferimenti in corso o in coda. Interromperli e "
                                    "chiudere?"))
               != QMessageBox::Yes)
        return;
    QDialog::reject();
}

void FileTransferDialog::updateControls()
{
    localView_->setEnabled(true);
    remoteView_->setEnabled(connected_);
    remotePathEdit_->setEnabled(connected_);
    remoteBookmarks_->setEnabled(connected_);
    uploadButton_->setEnabled(connected_ && !remoteDir_.isEmpty() && !selectedLocalPaths().isEmpty());
    downloadButton_->setEnabled(connected_ && !localDir_.isEmpty() && !selectedRemotePaths().isEmpty());
}

void FileTransferDialog::setStatus(const QString &text)
{
    statusLabel_->setText(text);
    statusLabel_->setToolTip(text);
}

QStringList FileTransferDialog::selectedLocalPaths() const
{
    QStringList paths;
    if (!localView_ || !localView_->selectionModel())
        return paths;
    for (const QModelIndex &index : localView_->selectionModel()->selectedRows(0)) {
        const QString path = localModel_->filePath(index);
        if (!path.isEmpty() && !paths.contains(path))
            paths.append(path);
    }
    return paths;
}

QStringList FileTransferDialog::selectedRemotePaths() const
{
    QStringList paths;
    for (const RemoteEntry &entry : selectedRemoteEntries())
        paths << entry.path;
    return paths;
}

QVector<RemoteEntry> FileTransferDialog::selectedRemoteEntries() const
{
    QVector<RemoteEntry> entries;
    if (!remoteView_ || !remoteView_->selectionModel())
        return entries;
    for (const QModelIndex &index : remoteView_->selectionModel()->selectedRows(0)) {
        const QString path = index.data(PathRole).toString();
        for (const RemoteEntry &entry : remoteEntries_) {
            if (entry.path == path) {
                entries << entry;
                break;
            }
        }
    }
    return entries;
}
