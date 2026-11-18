#include "mainwindow.h"

#include "commandpalette.h"
#include "connectiondialog.h"
#include "crypto.h"
#include "filetransferdialog.h"
#include "masterdialog.h"
#include "paths.h"
#include "sessionview.h"
#include "sshcommand.h"
#include "terminaldialog.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QDockWidget>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QScrollArea>
#include <QSet>
#include <QStringList>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <QInputDialog>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMdiArea>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QMdiSubWindow>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <qtermwidget.h>

#include <cmath>
#include <functional>

namespace {

bool removeNodeRecursive(Node &parent, const QString &id, Node *removed = nullptr)
{
    for (int i = 0; i < parent.children.size(); ++i) {
        const Node &child = parent.children.at(i);
        if (child.id == id) {
            if (removed)
                *removed = parent.children.takeAt(i);
            else
                parent.children.removeAt(i);
            return true;
        }
        if (child.kind == Node::Kind::Folder && removeNodeRecursive(parent.children[i], id, removed))
            return true;
    }
    return false;
}

void wipeSecrets(Node &node)
{
    if (node.kind == Node::Kind::Host)
        node.conn.clearSensitive();
    for (Node &child : node.children)
        wipeSecrets(child);
}

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle(tr("SSH Manager"));
    resize(1200, 780);

    // Remove helper files left behind by a previous run.
    QDir runDir(paths::runtimeDir());
    for (const QString &name :
         runDir.entryList({QStringLiteral("askpass-*.json"), QStringLiteral("key-*.tmp")}, QDir::Files)) {
        QFile::remove(runDir.filePath(name));
    }

    loadSettings();

    mdi_ = new QMdiArea(this);
    mdi_->setOption(QMdiArea::DontMaximizeSubWindowOnActivation, true);

    statusInfo_ = new QLabel(this);
    statusBar()->addPermanentWidget(statusInfo_);

    createActions();
    createDocks();

    // No menu bar: a borderless top dock holds the ribbon at full width, above
    // the side docks. Hiding the menu bar also stops styles (e.g. Breeze) from
    // creating and showing an empty one.
    menuBar()->hide();
    setCentralWidget(mdi_);

    auto *ribbonDock = new QDockWidget(this);
    ribbonDock->setObjectName(QStringLiteral("ribbonDock"));
    ribbonDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    ribbonDock->setTitleBarWidget(new QWidget(ribbonDock));
    ribbonDock->setWidget(createRibbon(ribbonDock));
    ribbonDock->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    addDockWidget(Qt::TopDockWidgetArea, ribbonDock);
    setCorner(Qt::TopLeftCorner, Qt::TopDockWidgetArea);
    setCorner(Qt::TopRightCorner, Qt::TopDockWidgetArea);

    const QColor ribbonText = palette().color(QPalette::WindowText);
    const QString disabledColor = QStringLiteral("rgba(%1, %2, %3, 0.55)")
                                      .arg(ribbonText.red())
                                      .arg(ribbonText.green())
                                      .arg(ribbonText.blue());
    const QString captionColor = QStringLiteral("rgba(%1, %2, %3, 0.72)")
                                     .arg(ribbonText.red())
                                     .arg(ribbonText.green())
                                     .arg(ribbonText.blue());

    setStyleSheet(QStringLiteral(
        "QTabWidget#ribbon::pane { border: 0; border-bottom: 1px solid palette(mid); }"
        "QTabWidget#ribbon QTabBar::tab { padding: 2px 12px; margin-right: 2px; border: 0;"
        " border-bottom: 2px solid transparent; }"
        "QTabWidget#ribbon QTabBar::tab:selected { border-bottom: 2px solid palette(highlight);"
        " font-weight: 600; }"
        "QTabWidget#ribbon QTabBar::tab:hover { background: palette(midlight); }"
        "QTabWidget#ribbon QToolButton { padding: 2px 5px; border: 1px solid transparent;"
        " border-radius: 3px; }"
        "QTabWidget#ribbon QToolButton:hover { background: palette(light);"
        " border-color: palette(midlight); }"
        "QTabWidget#ribbon QToolButton:pressed { background: palette(midlight); }"
        "QTabWidget#ribbon QToolButton:checked { background: palette(midlight);"
        " border-color: palette(mid); }"
        "QTabWidget#ribbon QToolButton:disabled { color: %1; }"
        "QTabWidget#ribbon QScrollBar:horizontal { height: 7px; background: transparent; }"
        "QTabWidget#ribbon QScrollBar::handle:horizontal { background: palette(mid);"
        " border-radius: 3px; min-width: 30px; }"
        "QTabWidget#ribbon QScrollBar::add-line:horizontal,"
        " QTabWidget#ribbon QScrollBar::sub-line:horizontal { width: 0; }"
        "QLabel#ribbonGroupCaption { color: %2; }"
        "QDockWidget::title { padding: 5px 6px; }"
        "QMdiArea { background: palette(window); }")
                      .arg(disabledColor, captionColor));

    // Size buttons against the final style/font so narrow ribbons scroll
    // instead of eliding their labels.
    if (auto *ribbon = findChild<QTabWidget *>(QStringLiteral("ribbon"))) {
        ribbon->ensurePolished();
        for (QToolButton *button : ribbon->findChildren<QToolButton *>()) {
            const int textWidth = button->fontMetrics().horizontalAdvance(button->text());
            button->setMinimumWidth(qMax(button->minimumWidth(),
                                         qMax(button->sizeHint().width(), textWidth + 20)));
        }
        ribbonDock->setMinimumHeight(ribbon->sizeHint().height());
    }

    autoLockTimer_ = new QTimer(this);
    autoLockTimer_->setSingleShot(true);
    connect(autoLockTimer_, &QTimer::timeout, this, [this] {
        if (locked_)
            return;
        // Do not clear the vault under an open modal dialog: close it first so
        // the editor cannot keep working with wiped data.
        if (QWidget *modal = QApplication::activeModalWidget())
            modal->close();
        lockVault();
        statusBar()->showMessage(tr("Archivio bloccato per inattività"), 8000);
        QTimer::singleShot(0, this, &MainWindow::requestUnlock);
    });

    qApp->installEventFilter(this);

    connect(mdi_, &QMdiArea::subWindowActivated, this, [this](QMdiSubWindow *) {
        updateSessionAction();
        highlightActiveSession();
        if (!locked_) {
            if (SessionView *session = activeSession())
                session->terminal()->setFocus();
        }
    });

    rebuildTree();
    applyLockedState();
    updateStatus();

    QTimer::singleShot(0, this, &MainWindow::initializeVault);
}

MainWindow::~MainWindow()
{
    qApp->removeEventFilter(this);
}

void MainWindow::loadSettings()
{
    QSettings settings;
    autoLockMinutes_ = settings.value(QStringLiteral("security/autoLockMinutes"), 15).toInt();
    if (autoLockMinutes_ < 0)
        autoLockMinutes_ = 0;
    favoritesOnly_ = settings.value(QStringLiteral("view/favoritesOnly"), false).toBool();
}

void MainWindow::createActions()
{
    unlockAction_ = new QAction(tr("Sblocca archivio…"), this);
    unlockAction_->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
    connect(unlockAction_, &QAction::triggered, this, &MainWindow::requestUnlock);

    lockAction_ = new QAction(tr("Blocca ora"), this);
    lockAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+L")));
    connect(lockAction_, &QAction::triggered, this, [this] {
        lockVault();
        statusBar()->showMessage(tr("Archivio bloccato"), 5000);
    });

    changePasswordAction_ = new QAction(tr("Cambia master password…"), this);
    connect(changePasswordAction_, &QAction::triggered, this,
            &MainWindow::changeMasterPassword);

    newConnectionAction_ = new QAction(style()->standardIcon(QStyle::SP_FileDialogNewFolder),
                                       tr("Nuova connessione…"), this);
    newConnectionAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+N")));
    connect(newConnectionAction_, &QAction::triggered, this,
            [this] { newConnection(selectedId()); });

    newFolderAction_ = new QAction(style()->standardIcon(QStyle::SP_DirIcon), tr("Nuova cartella…"),
                                   this);
    connect(newFolderAction_, &QAction::triggered, this, [this] { newFolder(selectedId()); });

    connectAction_ = new QAction(style()->standardIcon(QStyle::SP_MediaPlay), tr("Connetti"), this);
    connectAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Return")));
    connect(connectAction_, &QAction::triggered, this, [this] { openConnection(selectedId()); });

    localTerminalAction_ = new QAction(tr("Terminale locale"), this);
    localTerminalAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+T")));
    localTerminalAction_->setToolTip(tr("Apri una shell sul computer locale"));
    connect(localTerminalAction_, &QAction::triggered, this, &MainWindow::openLocalTerminal);

    quickAction_ = new QAction(tr("Comando rapido…"), this);
    quickAction_->setIconText(tr("Vai a…"));
    quickAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+K")));
    quickAction_->setToolTip(
        tr("Cerca connessioni e azioni, oppure apri una connessione rapida"));
    connect(quickAction_, &QAction::triggered, this, &MainWindow::showQuickPalette);

    editAction_ = new QAction(tr("Modifica…"), this);
    connect(editAction_, &QAction::triggered, this, [this] { editConnection(selectedId()); });

    deleteAction_ = new QAction(style()->standardIcon(QStyle::SP_TrashIcon), tr("Elimina"), this);
    deleteAction_->setShortcut(QKeySequence::Delete);
    connect(deleteAction_, &QAction::triggered, this, [this] { deleteNode(selectedId()); });

    duplicateAction_ = new QAction(tr("Duplica"), this);
    connect(duplicateAction_, &QAction::triggered, this,
            [this] { duplicateConnection(selectedId()); });

    renameAction_ = new QAction(tr("Rinomina…"), this);
    connect(renameAction_, &QAction::triggered, this, [this] { renameNode(selectedId()); });

    saveAction_ = new QAction(tr("Salva"), this);
    saveAction_->setShortcut(QKeySequence::Save);
    connect(saveAction_, &QAction::triggered, this, &MainWindow::persist);

    exportAction_ = new QAction(tr("Esporta archivio…"), this);
    connect(exportAction_, &QAction::triggered, this, &MainWindow::exportArchive);

    importAction_ = new QAction(tr("Importa archivio…"), this);
    connect(importAction_, &QAction::triggered, this, &MainWindow::importArchive);

    backupAction_ = new QAction(tr("Backup archivio…"), this);
    connect(backupAction_, &QAction::triggered, this, &MainWindow::backupArchive);

    quitAction_ = new QAction(tr("Esci"), this);
    quitAction_->setShortcut(QKeySequence::Quit);
    connect(quitAction_, &QAction::triggered, this, &QWidget::close);

    broadcastAction_ = new QAction(tr("Trasmetti input"), this);
    broadcastAction_->setCheckable(true);
    broadcastAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+B")));
    broadcastAction_->setToolTip(
        tr("Invia ogni tasto digitato a tutte le sessioni abilitate"));
    connect(broadcastAction_, &QAction::toggled, this, &MainWindow::updateBroadcastState);

    includeAction_ = new QAction(tr("Includi la sessione attiva nella trasmissione"), this);
    includeAction_->setCheckable(true);
    connect(includeAction_, &QAction::toggled, this, [this](bool checked) {
        if (SessionView *session = locked_ ? nullptr : activeSession())
            session->setIncludedInBroadcast(checked);
    });

    terminalAction_ = new QAction(tr("Personalizza terminale"), this);
    terminalAction_->setIconText(tr("Terminale"));
    terminalAction_->setToolTip(
        tr("Colori, font, cursore e cronologia della connessione attiva"));
    connect(terminalAction_, &QAction::triggered, this, &MainWindow::customizeActiveTerminal);

    transferAction_ = new QAction(tr("Trasferisci file…"), this);
    transferAction_->setIconText(tr("File"));
    transferAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")));
    transferAction_->setToolTip(tr("Invia o ricevi file dalla connessione attiva"));
    connect(transferAction_, &QAction::triggered, this, &MainWindow::transferFiles);

    favoriteAction_ = new QAction(tr("Preferito"), this);
    favoriteAction_->setCheckable(true);
    favoriteAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")));
    favoriteAction_->setToolTip(
        tr("Aggiungi o rimuovi la connessione selezionata dai preferiti"));
    connect(favoriteAction_, &QAction::toggled, this, &MainWindow::toggleFavorite);

    favoritesOnlyAction_ = new QAction(tr("Solo preferiti"), this);
    favoritesOnlyAction_->setCheckable(true);
    favoritesOnlyAction_->setChecked(favoritesOnly_);
    favoritesOnlyAction_->setToolTip(tr("Mostra solo le connessioni preferite"));
    connect(favoritesOnlyAction_, &QAction::toggled, this, [this](bool checked) {
        favoritesOnly_ = checked;
        QSettings settings;
        settings.setValue(QStringLiteral("view/favoritesOnly"), checked);
        applyFilter();
    });

    closeSessionAction_ = new QAction(tr("Chiudi sessione"), this);
    closeSessionAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+W")));
    connect(closeSessionAction_, &QAction::triggered, this, [this] {
        if (QMdiSubWindow *sub = mdi_->activeSubWindow())
            sub->close();
    });

    closeAllAction_ = new QAction(tr("Chiudi tutte le sessioni"), this);
    connect(closeAllAction_, &QAction::triggered, this, [this] {
        const auto windows = mdi_->subWindowList();
        for (QMdiSubWindow *sub : windows)
            sub->close();
    });

    tileAction_ = new QAction(tr("Disposizione automatica"), this);
    tileAction_->setIconText(tr("Automatica"));
    tileAction_->setToolTip(tr("Disponi le finestre nella griglia più adatta allo spazio"));
    connect(tileAction_, &QAction::triggered, this, [this] { mdi_->tileSubWindows(); });

    tileVerticalAction_ = new QAction(tr("Affianca verticalmente"), this);
    tileVerticalAction_->setIconText(tr("Verticale"));
    tileVerticalAction_->setToolTip(tr("Finestre una accanto all'altra, a colonne"));
    connect(tileVerticalAction_, &QAction::triggered, this,
            [this] { arrangeSessions(SessionLayout::Vertical); });

    tileHorizontalAction_ = new QAction(tr("Affianca orizzontalmente"), this);
    tileHorizontalAction_->setIconText(tr("Orizzontale"));
    tileHorizontalAction_->setToolTip(tr("Finestre una sopra l'altra, a righe"));
    connect(tileHorizontalAction_, &QAction::triggered, this,
            [this] { arrangeSessions(SessionLayout::Horizontal); });

    gridAction_ = new QAction(tr("Disponi a griglia"), this);
    gridAction_->setIconText(tr("Griglia"));
    gridAction_->setToolTip(tr("Finestre distribuite su una griglia uniforme"));
    connect(gridAction_, &QAction::triggered, this,
            [this] { arrangeSessions(SessionLayout::Grid); });

    cascadeAction_ = new QAction(tr("Sovrapponi finestre"), this);
    cascadeAction_->setIconText(tr("Sovrapponi"));
    connect(cascadeAction_, &QAction::triggered, this, [this] { mdi_->cascadeSubWindows(); });

    maximizeSessionAction_ = new QAction(style()->standardIcon(QStyle::SP_TitleBarMaxButton),
                                         tr("Massimizza finestra"), this);
    maximizeSessionAction_->setIconText(tr("Massimizza"));
    connect(maximizeSessionAction_, &QAction::triggered, this, [this] {
        if (QMdiSubWindow *sub = mdi_->activeSubWindow())
            sub->showMaximized();
    });

    restoreSessionAction_ = new QAction(style()->standardIcon(QStyle::SP_TitleBarNormalButton),
                                        tr("Ripristina finestra"), this);
    restoreSessionAction_->setIconText(tr("Ripristina"));
    connect(restoreSessionAction_, &QAction::triggered, this, [this] {
        if (QMdiSubWindow *sub = mdi_->activeSubWindow())
            sub->showNormal();
    });

    previousSessionAction_ = new QAction(style()->standardIcon(QStyle::SP_ArrowBack),
                                         tr("Finestra precedente"), this);
    previousSessionAction_->setIconText(tr("Precedente"));
    previousSessionAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+PgUp")));
    connect(previousSessionAction_, &QAction::triggered, this, [this] { cycleSession(-1); });

    nextSessionAction_ = new QAction(style()->standardIcon(QStyle::SP_ArrowForward),
                                     tr("Finestra successiva"), this);
    nextSessionAction_->setIconText(tr("Successiva"));
    nextSessionAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+PgDown")));
    connect(nextSessionAction_, &QAction::triggered, this, [this] { cycleSession(1); });

    includeAction_->setIconText(tr("Includi sessione"));
    closeAllAction_->setIconText(tr("Chiudi tutte"));
    changePasswordAction_->setIconText(tr("Cambia password"));
    newConnectionAction_->setIconText(tr("Nuova connessione"));

    const auto themed = [this](const QString &name, QStyle::StandardPixmap fallback) {
        const QIcon icon = QIcon::fromTheme(name);
        return icon.isNull() ? style()->standardIcon(fallback) : icon;
    };
    newConnectionAction_->setIcon(themed(QStringLiteral("list-add"),
                                         QStyle::SP_FileDialogNewFolder));
    newFolderAction_->setIcon(themed(QStringLiteral("folder-new"), QStyle::SP_DirIcon));
    editAction_->setIcon(
        themed(QStringLiteral("document-edit"), QStyle::SP_FileDialogDetailedView));
    duplicateAction_->setIcon(themed(QStringLiteral("edit-copy"), QStyle::SP_FileIcon));
    renameAction_->setIcon(
        themed(QStringLiteral("edit-rename"), QStyle::SP_FileDialogDetailedView));
    deleteAction_->setIcon(themed(QStringLiteral("edit-delete"), QStyle::SP_TrashIcon));
    connectAction_->setIcon(
        themed(QStringLiteral("utilities-terminal"), QStyle::SP_MediaPlay));
    localTerminalAction_->setIcon(
        themed(QStringLiteral("utilities-terminal"), QStyle::SP_ComputerIcon));
    quickAction_->setIcon(
        themed(QStringLiteral("system-search"), QStyle::SP_FileDialogContentsView));
    saveAction_->setIcon(
        themed(QStringLiteral("document-save"), QStyle::SP_DialogSaveButton));
    exportAction_->setIcon(
        themed(QStringLiteral("document-export"), QStyle::SP_DialogSaveButton));
    importAction_->setIcon(
        themed(QStringLiteral("document-import"), QStyle::SP_DialogOpenButton));
    backupAction_->setIcon(
        themed(QStringLiteral("document-save-as"), QStyle::SP_DriveHDIcon));
    unlockAction_->setIcon(
        themed(QStringLiteral("lock-open"), QStyle::SP_DialogOpenButton));
    lockAction_->setIcon(
        themed(QStringLiteral("system-lock-screen"), QStyle::SP_DialogCancelButton));
    changePasswordAction_->setIcon(
        themed(QStringLiteral("dialog-password"), QStyle::SP_MessageBoxInformation));
    broadcastAction_->setIcon(
        themed(QStringLiteral("network-transmit-receive"), QStyle::SP_MediaPlay));
    includeAction_->setIcon(
        themed(QStringLiteral("edit-select-all"), QStyle::SP_DialogApplyButton));
    terminalAction_->setIcon(
        themed(QStringLiteral("preferences-desktop-font"), QStyle::SP_FileDialogDetailedView));
    transferAction_->setIcon(themed(QStringLiteral("folder-remote"), QStyle::SP_DriveNetIcon));
    favoriteAction_->setIcon(themed(QStringLiteral("starred"), QStyle::SP_DialogApplyButton));
    favoritesOnlyAction_->setIcon(
        themed(QStringLiteral("starred"), QStyle::SP_DialogApplyButton));
    closeSessionAction_->setIcon(
        themed(QStringLiteral("window-close"), QStyle::SP_DialogCloseButton));
    closeAllAction_->setIcon(
        themed(QStringLiteral("window-close"), QStyle::SP_DialogCloseButton));
    // Layout icons are drawn explicitly: theme icons for these operations are
    // often missing or indistinguishable.
    const auto layoutIcon = [this](int kind) {
        QPixmap pixmap(48, 48);
        pixmap.fill(Qt::transparent);
        pixmap.setDevicePixelRatio(2);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.scale(2, 2);
        QColor color = palette().color(QPalette::WindowText);
        color.setAlphaF(0.78);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);

        const qreal r = 1.5;
        if (kind == 1) { // columns
            painter.drawRoundedRect(QRectF(3, 4, 8.5, 16), r, r);
            painter.drawRoundedRect(QRectF(12.5, 4, 8.5, 16), r, r);
        } else if (kind == 2) { // rows
            painter.drawRoundedRect(QRectF(3, 4, 18, 7), r, r);
            painter.drawRoundedRect(QRectF(3, 13, 18, 7), r, r);
        } else if (kind == 3) { // grid
            painter.drawRoundedRect(QRectF(3, 4, 8.5, 7), r, r);
            painter.drawRoundedRect(QRectF(12.5, 4, 8.5, 7), r, r);
            painter.drawRoundedRect(QRectF(3, 13, 8.5, 7), r, r);
            painter.drawRoundedRect(QRectF(12.5, 13, 8.5, 7), r, r);
        } else if (kind == 4) { // cascade
            QColor back = color;
            back.setAlphaF(0.30);
            painter.setBrush(back);
            painter.drawRoundedRect(QRectF(3, 3.5, 12, 12), r, r);
            painter.setBrush(color);
            painter.drawRoundedRect(QRectF(9, 9, 12, 12), r, r);
        } else { // automatic: one large pane plus two stacked
            painter.drawRoundedRect(QRectF(3, 4, 9, 16), r, r);
            painter.drawRoundedRect(QRectF(14, 4, 7, 7), r, r);
            painter.drawRoundedRect(QRectF(14, 13, 7, 7), r, r);
        }
        painter.end();
        return QIcon(pixmap);
    };

    tileAction_->setIcon(layoutIcon(0));
    tileVerticalAction_->setIcon(layoutIcon(1));
    tileHorizontalAction_->setIcon(layoutIcon(2));
    gridAction_->setIcon(layoutIcon(3));
    cascadeAction_->setIcon(layoutIcon(4));

    // Keep shortcuts working now that there is no menu bar: actions must be
    // registered on the window itself.
    const QList<QAction *> globalActions = {
        unlockAction_,           lockAction_,           changePasswordAction_,
        newConnectionAction_,    newFolderAction_,      connectAction_,
        localTerminalAction_,    quickAction_,          editAction_,             deleteAction_,
        duplicateAction_,
        renameAction_,           saveAction_,           exportAction_,
        importAction_,           backupAction_,         quitAction_,
        broadcastAction_,        includeAction_,        terminalAction_,
        transferAction_,         favoriteAction_,       favoritesOnlyAction_,
        closeSessionAction_,     closeAllAction_,
        tileAction_,             tileVerticalAction_,
        tileHorizontalAction_,   gridAction_,           cascadeAction_,
        maximizeSessionAction_,  restoreSessionAction_, previousSessionAction_,
        nextSessionAction_,
    };
    for (QAction *action : globalActions)
        addAction(action);

    aboutAction_ = new QAction(tr("Informazioni"), this);
    aboutAction_->setIcon(
        themed(QStringLiteral("help-about"), QStyle::SP_MessageBoxInformation));
    connect(aboutAction_, &QAction::triggered, this, &MainWindow::showAbout);

    updateSessionAction();
}

QWidget *MainWindow::createRibbon(QWidget *parent)
{
    auto *tabs = new QTabWidget(parent);
    tabs->setObjectName(QStringLiteral("ribbon"));
    tabs->setDocumentMode(true);
    tabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    tabs->tabBar()->setExpanding(false);

    auto makeButton = [](QAction *action, QWidget *parentWidget) {
        auto *button = new QToolButton(parentWidget);
        button->setDefaultAction(action);
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setIconSize(QSize(20, 20));
        button->setAutoRaise(true);
        // Keep the whole label: narrow ribbons scroll instead of eliding.
        button->setMinimumWidth(qMax(64, button->sizeHint().width()));
        button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        return button;
    };

    auto makeGroup = [&](const QString &caption, const QList<QWidget *> &items,
                         QWidget *parentWidget) {
        auto *group = new QWidget(parentWidget);
        auto *layout = new QVBoxLayout(group);
        layout->setContentsMargins(3, 1, 3, 0);
        layout->setSpacing(0);

        auto *row = new QHBoxLayout();
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(2);
        for (QWidget *item : items)
            row->addWidget(item);
        layout->addLayout(row, 1);

        auto *label = new QLabel(caption, group);
        label->setObjectName(QStringLiteral("ribbonGroupCaption"));
        label->setAlignment(Qt::AlignHCenter);
        QFont font = label->font();
        font.setPointSizeF(qMax(7.0, font.pointSizeF() - 2.0));
        label->setFont(font);
        layout->addWidget(label);
        return group;
    };

    auto makeSeparator = [](QWidget *parentWidget) {
        auto *separator = new QFrame(parentWidget);
        separator->setFrameShape(QFrame::VLine);
        separator->setFrameShadow(QFrame::Sunken);
        return separator;
    };

    auto makeAutoLockButton = [this](QWidget *parentWidget) {
        auto *button = new QToolButton(parentWidget);
        button->setText(tr("Blocco automatico"));
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setIcon(QIcon::fromTheme(QStringLiteral("preferences-system-time"),
                                          style()->standardIcon(QStyle::SP_MessageBoxInformation)));
        button->setIconSize(QSize(20, 20));
        button->setAutoRaise(true);
        button->setMinimumWidth(qMax(64, button->sizeHint().width()));
        button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        button->setPopupMode(QToolButton::InstantPopup);

        auto *menu = new QMenu(button);
        button->setMenu(menu);
        connect(menu, &QMenu::aboutToShow, this, [this, menu] {
            menu->clear();
            const QList<QPair<int, QString>> options = {
                {1, tr("Dopo 1 minuto")},   {5, tr("Dopo 5 minuti")},
                {15, tr("Dopo 15 minuti")}, {30, tr("Dopo 30 minuti")},
                {60, tr("Dopo 60 minuti")}, {0, tr("Mai")},
            };
            for (const QPair<int, QString> &option : options) {
                QAction *action = menu->addAction(option.second);
                action->setCheckable(true);
                action->setChecked(option.first == autoLockMinutes_);
                connect(action, &QAction::triggered, this,
                        [this, minutes = option.first] { setAutoLockMinutes(minutes); });
            }
        });
        return button;
    };

    auto addTab = [&](const QString &name, const QList<QWidget *> &groups) {
        auto *content = new QWidget;
        auto *layout = new QHBoxLayout(content);
        layout->setContentsMargins(3, 1, 3, 1);
        layout->setSpacing(2);
        for (int i = 0; i < groups.size(); ++i) {
            if (i > 0)
                layout->addWidget(makeSeparator(content));
            layout->addWidget(groups.at(i));
        }
        layout->addStretch();

        // Narrow windows get a thin horizontal scrollbar instead of clipped
        // ribbon buttons.
        auto *scroll = new QScrollArea(tabs);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setWidget(content);
        scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        // Reserve room for the horizontal scrollbar so captions never clip.
        scroll->setFixedHeight(content->sizeHint().height() + 9);
        tabs->addTab(scroll, name);
    };

    addTab(tr("Connessioni"),
           {            makeGroup(tr("Gestione"),
                      {makeButton(newConnectionAction_, tabs), makeButton(newFolderAction_, tabs),
                       makeButton(editAction_, tabs), makeButton(duplicateAction_, tabs),
                       makeButton(renameAction_, tabs), makeButton(deleteAction_, tabs)},
                      tabs),
            makeGroup(tr("Sessione"),
                      {makeButton(quickAction_, tabs), makeButton(connectAction_, tabs),
                       makeButton(localTerminalAction_, tabs)},
                      tabs)});

    addTab(tr("Archivio"),
           {makeGroup(tr("Dati"),
                      {makeButton(saveAction_, tabs), makeButton(exportAction_, tabs),
                       makeButton(importAction_, tabs), makeButton(backupAction_, tabs)},
                      tabs),
            makeGroup(tr("Sicurezza"),
                      {makeButton(unlockAction_, tabs), makeButton(lockAction_, tabs),
                       makeButton(changePasswordAction_, tabs), makeAutoLockButton(tabs)},
                      tabs)});

    addTab(tr("Sessioni"),
           {makeGroup(tr("Input"),
                      {makeButton(broadcastAction_, tabs), makeButton(includeAction_, tabs)}, tabs),
            makeGroup(tr("Terminale"), {makeButton(terminalAction_, tabs)}, tabs),
            makeGroup(tr("File"), {makeButton(transferAction_, tabs)}, tabs),
            makeGroup(tr("Finestre"),
                      {makeButton(closeSessionAction_, tabs), makeButton(closeAllAction_, tabs)},
                      tabs)});

    addTab(tr("Visualizza"),
           {makeGroup(tr("Disposizione"),
                      {makeButton(tileAction_, tabs), makeButton(tileVerticalAction_, tabs),
                       makeButton(tileHorizontalAction_, tabs), makeButton(gridAction_, tabs),
                       makeButton(cascadeAction_, tabs)},
                      tabs),
            makeGroup(tr("Finestra attiva"),
                      {makeButton(maximizeSessionAction_, tabs),
                       makeButton(restoreSessionAction_, tabs)},
                      tabs),
            makeGroup(tr("Navigazione"),
                      {makeButton(previousSessionAction_, tabs),
                       makeButton(nextSessionAction_, tabs)},
                      tabs),
            makeGroup(tr("Pannelli"),
                      {makeButton(connectionsDock_->toggleViewAction(), tabs),
                       makeButton(sessionDock_->toggleViewAction(), tabs)},
                      tabs)});

    addTab(tr("Aiuto"),
           {makeGroup(tr("Informazioni"), {makeButton(aboutAction_, tabs)}, tabs)});

    return tabs;
}

void MainWindow::setAutoLockMinutes(int minutes)
{
    autoLockMinutes_ = qMax(0, minutes);
    QSettings settings;
    settings.setValue(QStringLiteral("security/autoLockMinutes"), autoLockMinutes_);
    armAutoLock();
    statusBar()->showMessage(autoLockMinutes_ > 0
                                 ? tr("Blocco automatico dopo %1 minuti di inattività")
                                       .arg(autoLockMinutes_)
                                 : tr("Blocco automatico disattivato"),
                             5000);
}

void MainWindow::arrangeSessions(SessionLayout layout)
{
    const auto windows = mdi_->subWindowList(QMdiArea::StackingOrder);
    if (windows.isEmpty())
        return;

    const QRect area = mdi_->viewport()->rect();
    const int count = windows.size();
    int columns = 1;
    if (layout == SessionLayout::Vertical)
        columns = count;
    else if (layout == SessionLayout::Grid)
        columns = int(std::ceil(std::sqrt(double(count))));
    const int rows = (count + columns - 1) / columns;

    for (int i = 0; i < count; ++i) {
        QMdiSubWindow *sub = windows.at(i);
        sub->showNormal();
        const int row = i / columns;
        const int column = i % columns;
        const int x = area.width() * column / columns;
        const int y = area.height() * row / rows;
        const int width = area.width() * (column + 1) / columns - x;
        const int height = area.height() * (row + 1) / rows - y;
        sub->setGeometry(QRect(area.x() + x, area.y() + y, width, height));
    }
}

void MainWindow::cycleSession(int direction)
{
    const auto windows = mdi_->subWindowList(QMdiArea::StackingOrder);
    if (windows.isEmpty())
        return;

    const int count = windows.size();
    int index = windows.indexOf(mdi_->activeSubWindow());
    if (index < 0)
        index = direction > 0 ? -1 : 0;
    const int next = ((index + direction) % count + count) % count;

    mdi_->setActiveSubWindow(windows.at(next));
    windows.at(next)->raise();
}

void MainWindow::createDocks()
{
    auto *dock = new QDockWidget(tr("Connessioni"), this);
    dock->setObjectName(QStringLiteral("connectionsDock"));
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    auto *container = new QWidget(dock);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    filter_ = new QLineEdit(container);
    filter_->setPlaceholderText(tr("Filtra connessioni…"));
    filter_->setClearButtonEnabled(true);

    auto *filterButton = new QToolButton(container);
    filterButton->setObjectName(QStringLiteral("filterButton"));
    filterButton->setAutoRaise(true);
    filterButton->setPopupMode(QToolButton::InstantPopup);
    filterButton->setToolTip(tr("Filtri connessioni"));
    filterButton->setIcon(QIcon::fromTheme(
        QStringLiteral("view-filter"), style()->standardIcon(QStyle::SP_FileDialogDetailedView)));

    auto *filterMenu = new QMenu(filterButton);
    filterMenu->addAction(favoritesOnlyAction_);
    filterMenu->addSeparator();

    auto *tagMenu = filterMenu->addMenu(tr("Tag"));
    auto *tagGroup = new QActionGroup(tagMenu);
    tagGroup->setExclusive(true);
    connect(tagMenu, &QMenu::aboutToShow, this, [this, tagMenu, tagGroup] {
        tagMenu->clear();
        QAction *allTags = tagMenu->addAction(tr("Tutti"));
        allTags->setCheckable(true);
        allTags->setChecked(tagFilter_.isEmpty());
        tagGroup->addAction(allTags);
        connect(allTags, &QAction::triggered, this, [this] {
            tagFilter_.clear();
            applyFilter();
        });

        QStringList tags;
        QVector<const Connection *> connections;
        store_.collectConnections(connections);
        for (const Connection *connection : connections) {
            for (const QString &tag : connection->tags) {
                if (!tags.contains(tag, Qt::CaseInsensitive))
                    tags.append(tag);
            }
        }
        tags.sort(Qt::CaseInsensitive);
        for (const QString &tag : tags) {
            QAction *action = tagMenu->addAction(tag);
            action->setCheckable(true);
            action->setChecked(tag.compare(tagFilter_, Qt::CaseInsensitive) == 0);
            tagGroup->addAction(action);
            connect(action, &QAction::triggered, this, [this, tag] {
                tagFilter_ = tag;
                applyFilter();
            });
        }
    });

    QAction *clearFilters = filterMenu->addAction(tr("Azzera filtri"));
    connect(clearFilters, &QAction::triggered, this, [this] {
        filter_->clear();
        tagFilter_.clear();
        if (favoritesOnlyAction_->isChecked())
            favoritesOnlyAction_->setChecked(false);
        else
            applyFilter();
    });
    filterButton->setMenu(filterMenu);

    auto *filterRow = new QHBoxLayout();
    filterRow->setContentsMargins(0, 0, 0, 0);
    filterRow->setSpacing(4);
    filterRow->addWidget(filter_, 1);
    filterRow->addWidget(filterButton);

    tree_ = new QTreeWidget(container);
    tree_->setColumnCount(2);
    tree_->setHeaderLabels({tr("Nome"), tr("Destinazione")});
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setAlternatingRowColors(true);
    tree_->setUniformRowHeights(true);
    tree_->setAnimated(true);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree_->setRootIsDecorated(true);

    layout->addLayout(filterRow);
    layout->addWidget(tree_, 1);
    dock->setWidget(container);
    dock->setMinimumWidth(300);
    addDockWidget(Qt::LeftDockWidgetArea, dock);
    connectionsDock_ = dock;
    dock->toggleViewAction()->setIcon(
        QIcon::fromTheme(QStringLiteral("view-list-details"),
                         style()->standardIcon(QStyle::SP_FileDialogListView)));

    connect(filter_, &QLineEdit::textChanged, this, &MainWindow::applyFilter);
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, &MainWindow::updateNodeActions);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        if (locked_)
            return;
        const QString id = item->data(0, Qt::UserRole).toString();
        const Node *node = store_.findNode(id);
        if (!node)
            return;
        if (node->kind == Node::Kind::Host)
            openConnection(id);
        else
            item->setExpanded(!item->isExpanded());
    });
    connect(tree_, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
        if (locked_)
            return;
        if (Node *node = store_.findNode(item->data(0, Qt::UserRole).toString()))
            node->expanded = true;
    });
    connect(tree_, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *item) {
        if (locked_)
            return;
        if (Node *node = store_.findNode(item->data(0, Qt::UserRole).toString()))
            node->expanded = false;
    });
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        if (locked_)
            return;
        QTreeWidgetItem *item = tree_->itemAt(pos);
        if (!item)
            return;
        tree_->setCurrentItem(item);
        const Node *node = store_.findNode(item->data(0, Qt::UserRole).toString());
        if (!node)
            return;

        updateNodeActions();
        QMenu menu(this);
        if (node->kind == Node::Kind::Folder) {
            menu.addAction(newConnectionAction_);
            menu.addAction(newFolderAction_);
            menu.addSeparator();
            menu.addAction(renameAction_);
            menu.addAction(deleteAction_);
        } else {
            menu.addAction(connectAction_);
            menu.addAction(transferAction_);
            menu.addAction(favoriteAction_);
            menu.addAction(editAction_);
            menu.addAction(duplicateAction_);
            menu.addAction(renameAction_);
            menu.addSeparator();
            menu.addAction(deleteAction_);
        }
        menu.exec(tree_->viewport()->mapToGlobal(pos));
    });

    sessionDock_ = new QDockWidget(tr("Sessioni"), this);
    sessionDock_->setObjectName(QStringLiteral("sessionsDock"));
    sessionDock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    sessionList_ = new QTreeWidget(sessionDock_);
    sessionList_->setColumnCount(2);
    sessionList_->setHeaderLabels({tr("Sessione"), tr("Stato")});
    sessionList_->setRootIsDecorated(false);
    sessionList_->setAlternatingRowColors(true);
    sessionList_->setUniformRowHeights(true);
    sessionList_->setContextMenuPolicy(Qt::CustomContextMenu);
    sessionList_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    sessionList_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    sessionDock_->setWidget(sessionList_);
    sessionDock_->setMinimumWidth(260);
    sessionDock_->toggleViewAction()->setIcon(
        QIcon::fromTheme(QStringLiteral("view-list-text"),
                         style()->standardIcon(QStyle::SP_FileDialogDetailedView)));
    addDockWidget(Qt::LeftDockWidgetArea, sessionDock_);
    splitDockWidget(dock, sessionDock_, Qt::Vertical);

    connect(sessionList_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item, int) {
        if (!item || locked_)
            return;
        activateSession(
            reinterpret_cast<SessionView *>(item->data(0, Qt::UserRole).toULongLong()));
    });
    connect(sessionList_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item, int) {
        if (!item || locked_)
            return;
        activateSession(
            reinterpret_cast<SessionView *>(item->data(0, Qt::UserRole).toULongLong()));
    });
    connect(sessionList_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QTreeWidgetItem *item = sessionList_->itemAt(pos);
        if (!item || locked_)
            return;
        auto *session = reinterpret_cast<SessionView *>(item->data(0, Qt::UserRole).toULongLong());
        if (!session)
            return;

        QMenu menu(this);
        QAction *activate = menu.addAction(tr("Attiva"));
        QAction *close = menu.addAction(tr("Chiudi sessione"));
        QAction *chosen = menu.exec(sessionList_->viewport()->mapToGlobal(pos));
        if (chosen == activate) {
            activateSession(session);
        } else if (chosen == close) {
            const auto windows = mdi_->subWindowList();
            for (QMdiSubWindow *sub : windows) {
                if (sub->widget() == session) {
                    sub->close();
                    break;
                }
            }
        }
    });
}

void MainWindow::rebuildSessionList()
{
    if (!sessionList_)
        return;

    const SessionView *active = activeSession();
    sessionList_->clear();
    for (SessionView *session : sessions_) {
        auto *item = new QTreeWidgetItem(sessionList_);
        item->setText(0, session->title());

        QStringList state;
        state << (session->isFinished() ? tr("terminata") : tr("attiva"));
        if (!locked_ && broadcastAction_->isChecked() && session->isIncludedInBroadcast())
            state << QStringLiteral("TX");
        item->setText(1, state.join(QStringLiteral(" · ")));

        item->setIcon(0, style()->standardIcon(QStyle::SP_ComputerIcon));
        item->setToolTip(0, session->connection().target());
        item->setData(0, Qt::UserRole,
                      QVariant::fromValue<qulonglong>(reinterpret_cast<quintptr>(session)));

        if (session == active)
            sessionList_->setCurrentItem(item);
    }
}

void MainWindow::highlightActiveSession()
{
    if (!sessionList_)
        return;

    const SessionView *active = activeSession();
    for (int i = 0; i < sessionList_->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = sessionList_->topLevelItem(i);
        auto *session = reinterpret_cast<SessionView *>(item->data(0, Qt::UserRole).toULongLong());
        if (session == active) {
            sessionList_->setCurrentItem(item);
            return;
        }
    }
    sessionList_->clearSelection();
}

void MainWindow::activateSession(SessionView *session)
{
    if (!session)
        return;
    const auto windows = mdi_->subWindowList();
    for (QMdiSubWindow *sub : windows) {
        if (sub->widget() == session) {
            mdi_->setActiveSubWindow(sub);
            sub->raise();
            session->terminal()->setFocus();
            return;
        }
    }
}

QTreeWidgetItem *MainWindow::addNodeItem(QTreeWidgetItem *parentItem, const Node &node)
{
    auto *item = parentItem ? new QTreeWidgetItem(parentItem) : new QTreeWidgetItem(tree_);
    item->setText(0, node.name.isEmpty() ? tr("(senza nome)") : node.name);
    item->setData(0, Qt::UserRole, node.id);
    item->setData(0, Qt::UserRole + 1, static_cast<int>(node.kind));

    if (node.kind == Node::Kind::Folder) {
        item->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
        int hosts = 0;
        std::function<void(const Node &)> countHosts = [&](const Node &current) {
            for (const Node &child : current.children) {
                if (child.kind == Node::Kind::Host)
                    ++hosts;
                else
                    countHosts(child);
            }
        };
        countHosts(node);
        item->setText(1, tr("%n connessione/i", "", hosts));
        for (const Node &child : node.children)
            addNodeItem(item, child);
        item->setExpanded(node.expanded);
    } else {
        const QIcon hostIcon =
            node.conn.favorite
                ? QIcon::fromTheme(QStringLiteral("starred"),
                                   style()->standardIcon(QStyle::SP_DialogApplyButton))
                : style()->standardIcon(QStyle::SP_ComputerIcon);
        item->setIcon(0, hostIcon);
        item->setText(1, node.conn.target());
        item->setData(0, Qt::UserRole + 3, node.conn.favorite);
        item->setData(0, Qt::UserRole + 4, node.conn.tags);
        QString tip = node.conn.target();
        if (!node.conn.keyName.isEmpty())
            tip += QStringLiteral("\nChiave archiviata: ") + node.conn.keyName;
        if (!node.conn.tags.isEmpty())
            tip += QStringLiteral("\nTag: ") + node.conn.tags.join(QStringLiteral(", "));
        if (!node.conn.notes.isEmpty())
            tip += QStringLiteral("\n") + node.conn.notes;
        item->setToolTip(0, tip);
        item->setToolTip(1, tip);
    }

    itemById_.insert(node.id, item);
    return item;
}

void MainWindow::rebuildTree(const QString &selectId)
{
    itemById_.clear();
    tree_->clear();
    for (const Node &child : store_.root.children)
        addNodeItem(nullptr, child);

    if (!selectId.isEmpty()) {
        if (QTreeWidgetItem *item = itemById_.value(selectId)) {
            tree_->setCurrentItem(item);
            tree_->scrollToItem(item);
        }
    }
    applyFilter();
    updateNodeActions();
}

void MainWindow::applyFilter()
{
    const QString needle = filter_ ? filter_->text().trimmed() : QString();

    std::function<bool(QTreeWidgetItem *)> apply = [&](QTreeWidgetItem *item) -> bool {
        bool childMatches = false;
        for (int i = 0; i < item->childCount(); ++i) {
            if (apply(item->child(i)))
                childMatches = true;
        }
        const bool favorite = item->data(0, Qt::UserRole + 3).toBool();
        const QStringList itemTags = item->data(0, Qt::UserRole + 4).toStringList();
        const bool tagMatches =
            tagFilter_.isEmpty() || itemTags.contains(tagFilter_, Qt::CaseInsensitive);
        const bool selfMatches =
            (!favoritesOnly_ || favorite) && tagMatches
            && (needle.isEmpty() || item->text(0).contains(needle, Qt::CaseInsensitive)
                || item->text(1).contains(needle, Qt::CaseInsensitive)
                || itemTags.join(QLatin1Char(' ')).contains(needle, Qt::CaseInsensitive));
        item->setHidden(!(selfMatches || childMatches));
        if ((!needle.isEmpty() || favoritesOnly_) && (selfMatches || childMatches))
            item->setExpanded(true);
        return selfMatches || childMatches;
    };

    for (int i = 0; i < tree_->topLevelItemCount(); ++i)
        apply(tree_->topLevelItem(i));
}

void MainWindow::updateStatus()
{
    if (locked_) {
        statusInfo_->setText(tr("Archivio bloccato"));
        return;
    }
    const bool active = broadcastAction_ && broadcastAction_->isChecked();
    statusInfo_->setText(tr("Sessioni: %1  ·  Trasmissione input: %2")
                             .arg(sessions_.size())
                             .arg(active ? tr("attiva") : tr("disattivata")));
}

bool MainWindow::persist()
{
    if (locked_ || !vault_.isUnlocked())
        return false;

    QByteArray payload = QJsonDocument(store_.toJson()).toJson(QJsonDocument::Compact);
    QString error;
    const bool ok = vault_.save(payload, &error);
    crypto::cleanse(payload);
    if (!ok)
        QMessageBox::warning(this, tr("Salvataggio non riuscito"), error);
    return ok;
}

void MainWindow::initializeVault()
{
    if (vault_.exists()) {
        requestUnlock();
        return;
    }

    if (QFileInfo::exists(paths::storeFile())) {
        const auto answer = QMessageBox::question(
            this, tr("Migrazione archivio"),
            tr("Esiste un archivio delle versioni precedenti (connessioni e chiavi cifrate con la "
               "chiave locale).\n\nVerrà importato nel nuovo database protetto da master password e "
               "i file precedenti saranno eliminati.\n\nContinuare?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (answer != QMessageBox::Yes) {
            statusBar()->showMessage(tr("Archivio bloccato"), 5000);
            return;
        }
    }

    createVault();
}

void MainWindow::createVault()
{
    MasterPasswordDialog dialog(MasterPasswordDialog::Mode::Create, this);
    while (dialog.exec() == QDialog::Accepted) {
        QByteArray password = dialog.password();
        QByteArray payload;
        QString error;
        bool migrating = false;

        if (QFileInfo::exists(paths::storeFile())) {
            ConnectionStore legacy;
            if (legacy.loadLegacyFile(paths::storeFile(), &error)) {
                payload = QJsonDocument(legacy.toJson()).toJson(QJsonDocument::Compact);
                migrating = true;
            } else {
                legacy.clearSensitive();
                crypto::cleanse(password);
                crypto::cleanse(payload);
                QMessageBox::warning(this, tr("Migrazione non riuscita"), error);
                return;
            }
            legacy.clearSensitive();
        } else {
            ConnectionStore empty;
            payload = QJsonDocument(empty.toJson()).toJson(QJsonDocument::Compact);
            empty.clearSensitive();
        }

        if (!vault_.create(password, payload, &error)) {
            crypto::cleanse(password);
            crypto::cleanse(payload);
            dialog.setError(error);
            dialog.clearFields();
            continue;
        }
        crypto::cleanse(password);

        if (migrating) {
            QFile::remove(paths::storeFile());
            QFile::remove(paths::keyFile());
        }

        finishUnlock(payload);
        crypto::cleanse(payload);
        return;
    }

    statusBar()->showMessage(tr("Archivio non creato"), 5000);
}

void MainWindow::requestUnlock()
{
    if (!vault_.exists()) {
        createVault();
        return;
    }
    if (!locked_)
        return;

    MasterPasswordDialog dialog(MasterPasswordDialog::Mode::Unlock, this);
    while (dialog.exec() == QDialog::Accepted) {
        QByteArray password = dialog.password();
        QByteArray payload;
        QString error;
        const bool ok = vault_.unlock(password, &payload, &error);
        crypto::cleanse(password);
        if (ok) {
            finishUnlock(payload);
            crypto::cleanse(payload);
            return;
        }
        crypto::cleanse(payload);
        dialog.setError(error);
        dialog.clearFields();
    }
}

void MainWindow::finishUnlock(QByteArray payload)
{
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    QString error;
    const bool ok = !document.isNull() && store_.fromJson(document.object(), &error);
    crypto::cleanse(payload);

    if (!ok) {
        QMessageBox::critical(this, tr("Archivio non valido"),
                              error.isEmpty() ? parseError.errorString() : error);
        store_.clearSensitive();
        store_.reset();
        vault_.lock();
        locked_ = true;
        applyLockedState();
        updateStatus();
        return;
    }

    locked_ = false;
    rebuildTree();
    rebuildSessionList();
    applyLockedState();
    updateStatus();
    armAutoLock();
    statusBar()->showMessage(tr("Archivio sbloccato"), 3000);
}

void MainWindow::lockVault()
{
    if (locked_)
        return;

    const auto windows = mdi_->subWindowList();
    for (QMdiSubWindow *sub : windows)
        sub->close();

    store_.clearSensitive();
    store_.reset();
    vault_.lock();

    locked_ = true;
    if (autoLockTimer_)
        autoLockTimer_->stop();

    rebuildTree();
    if (sessionList_)
        sessionList_->clear();
    applyLockedState();
    updateStatus();
}

void MainWindow::applyLockedState()
{
    const bool unlocked = !locked_;
    setWindowTitle(unlocked ? tr("SSH Manager") : tr("SSH Manager — bloccato"));

    filter_->setEnabled(unlocked);
    favoritesOnlyAction_->setEnabled(unlocked);
    tree_->setEnabled(unlocked);
    if (sessionList_)
        sessionList_->setEnabled(unlocked);
    newConnectionAction_->setEnabled(unlocked);
    newFolderAction_->setEnabled(unlocked);
    localTerminalAction_->setEnabled(unlocked);
    quickAction_->setEnabled(unlocked);
    saveAction_->setEnabled(unlocked);
    exportAction_->setEnabled(unlocked);
    importAction_->setEnabled(unlocked);
    backupAction_->setEnabled(unlocked);
    broadcastAction_->setEnabled(unlocked);
    unlockAction_->setEnabled(locked_);
    lockAction_->setEnabled(unlocked);
    changePasswordAction_->setEnabled(unlocked);
    updateSessionAction();
    updateNodeActions();
}

void MainWindow::updateNodeActions()
{
    const Node *node = locked_ ? nullptr : store_.findNode(selectedId());
    const bool isHost = node && node->kind == Node::Kind::Host;
    connectAction_->setEnabled(isHost);
    editAction_->setEnabled(isHost);
    duplicateAction_->setEnabled(isHost);
    deleteAction_->setEnabled(node != nullptr);
    renameAction_->setEnabled(node != nullptr);
    favoriteAction_->setEnabled(isHost);
    {
        const QSignalBlocker blocker(favoriteAction_);
        favoriteAction_->setChecked(isHost && node->conn.favorite);
    }
    updateTransferAction();
}

void MainWindow::toggleFavorite(bool favorite)
{
    if (locked_)
        return;

    Connection *connection = store_.findConnection(selectedId());
    if (!connection || connection->favorite == favorite)
        return;

    connection->favorite = favorite;
    if (!persist())
        return;

    rebuildTree(connection->id);
    statusBar()->showMessage(favorite ? tr("Aggiunta ai preferiti")
                                      : tr("Rimossa dai preferiti"),
                             3000);
}

void MainWindow::updateTransferAction()
{
    bool available = false;
    if (!locked_) {
        if (SessionView *session = activeSession();
            session && !session->connection().local
            && store_.findConnection(session->connection().id))
            available = true;
        if (!available) {
            const Node *node = store_.findNode(selectedId());
            available = node && node->kind == Node::Kind::Host;
        }
    }
    transferAction_->setEnabled(available);
}

void MainWindow::armAutoLock()
{
    if (!autoLockTimer_)
        return;
    autoLockTimer_->stop();
    if (!locked_ && autoLockMinutes_ > 0)
        autoLockTimer_->start(autoLockMinutes_ * 60 * 1000);
}

void MainWindow::changeMasterPassword()
{
    if (locked_)
        return;

    MasterPasswordDialog dialog(MasterPasswordDialog::Mode::Change, this);
    while (dialog.exec() == QDialog::Accepted) {
        QByteArray current = dialog.currentPassword();
        QByteArray next = dialog.password();
        QByteArray payload = QJsonDocument(store_.toJson()).toJson(QJsonDocument::Compact);
        QString error;
        const bool ok = vault_.changePassword(current, next, payload, &error);
        crypto::cleanse(current);
        crypto::cleanse(next);
        crypto::cleanse(payload);

        if (ok) {
            QMessageBox::information(this, tr("Master password aggiornata"),
                                     tr("L'archivio è stato ricifrato con la nuova password."));
            armAutoLock();
            return;
        }

        dialog.setError(error);
        dialog.clearFields();
    }
}

void MainWindow::exportArchive()
{
    if (locked_ || !vault_.isUnlocked())
        return;

    const QString suggested = QDir::homePath() + QStringLiteral("/sshmanager-export-")
                              + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmm"))
                              + QStringLiteral(".db");
    const QString path =
        QFileDialog::getSaveFileName(this, tr("Esporta archivio"), suggested,
                                     tr("Archivio SSHManager (*.db);;Tutti i file (*)"));
    if (path.isEmpty())
        return;
    if (QFileInfo(path).canonicalFilePath() == QFileInfo(vault_.path()).canonicalFilePath()) {
        QMessageBox::warning(this, tr("Esporta archivio"),
                             tr("Scegli un percorso diverso dall'archivio in uso."));
        return;
    }

    MasterPasswordDialog dialog(MasterPasswordDialog::Mode::Create, this);
    dialog.setWindowTitle(tr("Esporta archivio"));
    dialog.setTitleText(tr("Password di esportazione"));
    dialog.setDescriptionText(
        tr("Il file esportato conterrà connessioni e chiavi cifrate con questa password. "
           "Servirà per importarlo, anche su un altro computer."));
    if (dialog.exec() != QDialog::Accepted)
        return;

    QByteArray password = dialog.password();
    QByteArray payload = QJsonDocument(store_.toJson()).toJson(QJsonDocument::Compact);
    QString error;
    const bool ok = Vault::writeArchive(path, password, payload, &error);
    crypto::cleanse(password);
    crypto::cleanse(payload);
    if (!ok) {
        QMessageBox::critical(this, tr("Esportazione non riuscita"), error);
        return;
    }

    statusBar()->showMessage(tr("Archivio esportato in %1").arg(path), 8000);
}

void MainWindow::importArchive()
{
    if (locked_ || !vault_.isUnlocked())
        return;

    const QString path =
        QFileDialog::getOpenFileName(this, tr("Importa archivio"), QDir::homePath(),
                                     tr("Archivio SSHManager (*.db);;Tutti i file (*)"));
    if (path.isEmpty())
        return;
    if (QFileInfo(path).canonicalFilePath() == QFileInfo(vault_.path()).canonicalFilePath()) {
        QMessageBox::warning(this, tr("Importa archivio"),
                             tr("Il file scelto è l'archivio attualmente aperto."));
        return;
    }

    QByteArray payload;
    QString error;
    MasterPasswordDialog dialog(MasterPasswordDialog::Mode::Unlock, this);
    dialog.setWindowTitle(tr("Importa archivio"));
    dialog.setTitleText(tr("Password del file da importare"));
    dialog.setDescriptionText(tr("Inserisci la password usata per esportare il file."));
    while (true) {
        if (dialog.exec() != QDialog::Accepted)
            return;

        QByteArray password = dialog.password();
        Vault imported(path);
        const bool ok = imported.unlock(password, &payload, &error);
        crypto::cleanse(password);
        if (ok)
            break;

        crypto::cleanse(payload);
        dialog.setError(error);
        dialog.clearFields();
    }

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    crypto::cleanse(payload);
    if (document.isNull() || !document.isObject()) {
        QMessageBox::critical(this, tr("Importazione non riuscita"),
                              tr("Il file non contiene dati validi: %1")
                                  .arg(parseError.errorString()));
        return;
    }

    ConnectionStore preview;
    QString previewError;
    if (!preview.fromJson(document.object(), &previewError)) {
        preview.clearSensitive();
        QMessageBox::critical(this, tr("Importazione non riuscita"), previewError);
        return;
    }
    QVector<const Connection *> connections;
    preview.collectConnections(connections);
    const int count = connections.size();
    preview.clearSensitive();

    const auto answer = QMessageBox::question(
        this, tr("Importa archivio"),
        tr("L'archivio attuale verrà sostituito con quello importato (%1 connessioni).\n\n"
           "Le sessioni già aperte resteranno attive. Continuare?")
            .arg(count),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    if (!store_.fromJson(document.object(), &error)) {
        QMessageBox::critical(this, tr("Importazione non riuscita"), error);
        return;
    }

    if (!persist())
        return;

    rebuildTree();
    updateStatus();
    statusBar()->showMessage(tr("Archivio importato (%1 connessioni)").arg(count), 8000);
}

void MainWindow::backupArchive()
{
    if (locked_ || !vault_.isUnlocked())
        return;

    const QString suggested = QDir::homePath() + QStringLiteral("/sshmanager-backup-")
                              + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmm"))
                              + QStringLiteral(".db");
    const QString path =
        QFileDialog::getSaveFileName(this, tr("Backup archivio"), suggested,
                                     tr("Archivio SSHManager (*.db);;Tutti i file (*)"));
    if (path.isEmpty())
        return;
    if (QFileInfo(path).canonicalFilePath() == QFileInfo(vault_.path()).canonicalFilePath()) {
        QMessageBox::warning(this, tr("Backup archivio"),
                             tr("Scegli un percorso diverso dall'archivio in uso."));
        return;
    }

    if (!persist())
        return;

    QFile::remove(path);
    if (!QFile::copy(vault_.path(), path)) {
        QMessageBox::critical(this, tr("Backup non riuscito"),
                              tr("Impossibile copiare l'archivio in %1.").arg(path));
        return;
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    statusBar()->showMessage(tr("Backup salvato in %1").arg(path), 8000);
}

void MainWindow::customizeActiveTerminal()
{
    if (locked_ || !vault_.isUnlocked())
        return;

    SessionView *session = activeSession();
    if (!session)
        return;
    Connection *stored = store_.findConnection(session->connection().id);
    if (!stored && !session->connection().local)
        return;

    TerminalDialog dialog(stored ? stored->terminal : session->connection().terminal, this);
    dialog.setWindowTitle(tr("Terminale — %1").arg(session->connection().displayName()));
    if (dialog.exec() != QDialog::Accepted)
        return;

    const TerminalProfile profile = dialog.profile();
    if (stored) {
        stored->terminal = profile;
        if (!persist())
            return;
    }
    session->applyTerminalProfile(profile);
    statusBar()->showMessage(tr("Impostazioni terminale aggiornate"), 4000);
}

void MainWindow::updateSessionAction()
{
    SessionView *session = locked_ ? nullptr : activeSession();
    includeAction_->setEnabled(session != nullptr);
    terminalAction_->setEnabled(session != nullptr);
    closeSessionAction_->setEnabled(session != nullptr);
    maximizeSessionAction_->setEnabled(session != nullptr);
    restoreSessionAction_->setEnabled(session != nullptr);
    nextSessionAction_->setEnabled(sessions_.size() > 1);
    previousSessionAction_->setEnabled(sessions_.size() > 1);
    const bool hasSessions = !locked_ && !sessions_.isEmpty();
    closeAllAction_->setEnabled(hasSessions);
    tileAction_->setEnabled(hasSessions);
    tileVerticalAction_->setEnabled(hasSessions);
    tileHorizontalAction_->setEnabled(hasSessions);
    gridAction_->setEnabled(hasSessions);
    cascadeAction_->setEnabled(hasSessions);
    if (session) {
        const QSignalBlocker blocker(includeAction_);
        includeAction_->setChecked(session->isIncludedInBroadcast());
    }
    updateTransferAction();
}

QString MainWindow::selectedId() const
{
    QTreeWidgetItem *item = tree_ ? tree_->currentItem() : nullptr;
    return item ? item->data(0, Qt::UserRole).toString() : QString();
}

SessionView *MainWindow::activeSession() const
{
    if (!mdi_)
        return nullptr;
    QMdiSubWindow *sub = mdi_->activeSubWindow();
    return sub ? qobject_cast<SessionView *>(sub->widget()) : nullptr;
}

bool MainWindow::resolveConnection(const QString &id, Connection *effective,
                                   QVector<Connection> *chain)
{
    const Connection *stored = store_.findConnection(id);
    if (!stored)
        return false;

    *effective = *stored;
    if (effective->auth == AuthType::Password && effective->password.isEmpty()) {
        bool ok = false;
        const QString password = QInputDialog::getText(
            this, tr("Password richiesta"),
            tr("Password per %1:").arg(effective->target()), QLineEdit::Password, QString(), &ok);
        if (!ok)
            return false;
        effective->password = password.toUtf8();
    }

    chain->clear();
    QSet<QString> visited;
    visited.insert(id);
    QString nextId = effective->jumpId;
    while (!nextId.isEmpty() && !visited.contains(nextId)) {
        const Connection *hop = store_.findConnection(nextId);
        if (!hop)
            break;
        visited.insert(nextId);
        chain->append(*hop);
        nextId = hop->jumpId;
    }
    if (!effective->jumpId.isEmpty() && chain->isEmpty()) {
        QMessageBox::warning(this, tr("Jump host mancante"),
                             tr("Il jump host configurato non esiste più. Modifica la connessione."));
        effective->clearSensitive();
        return false;
    }
    return true;
}

void MainWindow::openConnection(const QString &id)
{
    if (locked_)
        return;

    Connection effective;
    QVector<Connection> chain;
    if (!resolveConnection(id, &effective, &chain))
        return;

    const SshPlan plan = buildSshPlan(effective, chain, paths::runtimeDir());
    effective.clearSensitive();
    for (Connection &hop : chain)
        hop.clearSensitive();

    if (!plan.valid) {
        for (const QString &path : plan.temporaryFiles)
            QFile::remove(path);
        QMessageBox::warning(this, tr("Impossibile avviare la sessione"), plan.error);
        return;
    }
    if (!QFileInfo::exists(plan.program)) {
        for (const QString &path : plan.temporaryFiles)
            QFile::remove(path);
        QMessageBox::critical(this, tr("OpenSSH non trovato"),
                              tr("Il client \"ssh\" non è stato trovato in %1.").arg(plan.program));
        return;
    }

    rememberRecentConnection(id);
    startSession(effective, plan);
}

void MainWindow::transferFiles()
{
    if (locked_ || !vault_.isUnlocked())
        return;

    QString id;
    if (SessionView *session = activeSession();
        session && !session->connection().local
        && store_.findConnection(session->connection().id))
        id = session->connection().id;
    if (id.isEmpty()) {
        const Node *node = store_.findNode(selectedId());
        if (node && node->kind == Node::Kind::Host)
            id = node->id;
    }
    if (id.isEmpty()) {
        QMessageBox::information(this, tr("Trasferimento file"),
                                 tr("Seleziona una connessione o apri una sessione remota."));
        return;
    }

    Connection effective;
    QVector<Connection> chain;
    if (!resolveConnection(id, &effective, &chain))
        return;

    const SftpPlan plan = buildSftpPlan(effective, chain, paths::runtimeDir());
    effective.clearSensitive();
    for (Connection &hop : chain)
        hop.clearSensitive();

    if (!plan.valid) {
        for (const QString &path : plan.temporaryFiles)
            QFile::remove(path);
        QMessageBox::warning(this, tr("Trasferimento file"), plan.error);
        return;
    }
    if (!QFileInfo::exists(plan.program)) {
        for (const QString &path : plan.temporaryFiles)
            QFile::remove(path);
        QMessageBox::critical(this, tr("OpenSSH non trovato"),
                              tr("Il client \"ssh\" non è stato trovato in %1.")
                                  .arg(plan.program));
        return;
    }

    FileTransferDialog dialog(effective, plan, this);
    dialog.exec();
}

void MainWindow::openLocalTerminal()
{
    if (locked_)
        return;

    Connection local;
    local.id = ConnectionStore::newId();
    local.local = true;
    local.name = tr("Terminale locale");
    local.host = QStringLiteral("localhost");
    local.user = qEnvironmentVariable("USER", qEnvironmentVariable("USERNAME"));
    local.auth = AuthType::Agent;

    SshPlan plan;
    plan.valid = true;
#ifdef Q_OS_WIN
    plan.program = qEnvironmentVariable("COMSPEC", QStringLiteral("cmd.exe"));
#else
    plan.program = qEnvironmentVariable("SHELL", QStringLiteral("/bin/sh"));
    if (!QFileInfo::exists(plan.program))
        plan.program = QStringLiteral("/bin/sh");
#endif
    startSession(local, plan);
}

void MainWindow::showQuickPalette()
{
    if (locked_ || !vault_.isUnlocked())
        return;

    QSettings settings;
    const QStringList recentIds =
        settings.value(QStringLiteral("palette/recentConnections")).toStringList();
    const QStringList recentTargets =
        settings.value(QStringLiteral("palette/recentQuickTargets")).toStringList();

    QVector<PaletteEntry> entries;
    QSet<QString> seen;
    const QIcon connectionIcon = style()->standardIcon(QStyle::SP_ComputerIcon);
    const QIcon favoriteIcon = QIcon::fromTheme(QStringLiteral("starred"), connectionIcon);
    const QIcon quickIcon = QIcon::fromTheme(QStringLiteral("network-server"), connectionIcon);

    for (const QString &id : recentIds) {
        const Connection *connection = store_.findConnection(id);
        if (!connection || seen.contains(id))
            continue;
        seen.insert(id);
        PaletteEntry entry;
        entry.kind = PaletteEntry::Kind::RecentConnection;
        entry.id = id;
        entry.title = connection->displayName();
        entry.detail = tr("Recente · %1").arg(connection->target());
        if (!connection->tags.isEmpty())
            entry.detail += QStringLiteral(" · ") + connection->tags.join(QStringLiteral(", "));
        entry.favorite = connection->favorite;
        entry.icon = entry.favorite ? favoriteIcon : connectionIcon;
        entries.append(entry);
    }

    for (const QString &target : recentTargets) {
        QString user;
        QString host;
        QString normalized;
        quint16 port = 22;
        if (!parseQuickTarget(target, &user, &host, &port, &normalized))
            continue;
        PaletteEntry entry;
        entry.kind = PaletteEntry::Kind::QuickConnect;
        entry.title = normalized;
        entry.detail = tr("Connessione rapida recente");
        entry.target = normalized;
        entry.icon = quickIcon;
        entries.append(entry);
    }

    std::function<void(const Node &, const QStringList &)> walk =
        [&](const Node &node, const QStringList &path) {
            for (const Node &child : node.children) {
                if (child.kind == Node::Kind::Folder) {
                    QStringList next = path;
                    if (!child.name.isEmpty())
                        next.append(child.name);
                    walk(child, next);
                    continue;
                }
                if (seen.contains(child.id))
                    continue;
                seen.insert(child.id);
                PaletteEntry entry;
                entry.kind = PaletteEntry::Kind::Connection;
                entry.id = child.id;
                entry.title = child.conn.displayName();
                entry.detail = child.conn.target();
                if (!path.isEmpty())
                    entry.detail += QStringLiteral(" · ") + path.join(QStringLiteral(" / "));
                if (!child.conn.tags.isEmpty())
                    entry.detail += QStringLiteral(" · ")
                                    + child.conn.tags.join(QStringLiteral(", "));
                entry.favorite = child.conn.favorite;
                entry.icon = entry.favorite ? favoriteIcon : connectionIcon;
                entries.append(entry);
            }
        };
    walk(store_.root, QStringList());

    const QList<QAction *> actions = {
        newConnectionAction_, newFolderAction_, connectAction_, localTerminalAction_,
        editAction_, duplicateAction_, renameAction_, terminalAction_, transferAction_,
        broadcastAction_, includeAction_, saveAction_, exportAction_, importAction_,
        backupAction_, lockAction_, changePasswordAction_, closeSessionAction_,
        closeAllAction_, tileAction_, tileVerticalAction_, tileHorizontalAction_,
        gridAction_, cascadeAction_, maximizeSessionAction_, restoreSessionAction_,
        previousSessionAction_, nextSessionAction_, aboutAction_,
    };
    for (QAction *action : actions) {
        if (!action || !action->isEnabled())
            continue;
        PaletteEntry entry;
        entry.kind = PaletteEntry::Kind::Action;
        entry.title = action->iconText();
        entry.detail = tr("Azione");
        entry.shortcut = action->shortcut().toString(QKeySequence::NativeText);
        entry.icon = action->icon();
        entry.action = action;
        entries.append(entry);
    }

    CommandPalette palette(this);
    palette.setEntries(entries);
    if (palette.exec() != QDialog::Accepted)
        return;

    const PaletteEntry chosen = palette.selectedEntry();
    switch (chosen.kind) {
    case PaletteEntry::Kind::Connection:
    case PaletteEntry::Kind::RecentConnection:
        if (!chosen.id.isEmpty()) {
            rebuildTree(chosen.id);
            openConnection(chosen.id);
        }
        break;
    case PaletteEntry::Kind::QuickConnect:
        connectQuick(chosen.target);
        break;
    case PaletteEntry::Kind::Action:
        if (chosen.action)
            chosen.action->trigger();
        break;
    }
}

void MainWindow::connectQuick(const QString &target)
{
    if (locked_)
        return;

    QString user;
    QString host;
    QString normalized;
    quint16 port = 22;
    if (!parseQuickTarget(target, &user, &host, &port, &normalized)) {
        QMessageBox::warning(this, tr("Connessione rapida"),
                             tr("Destinazione non valida: usa host, utente@host o host:porta."));
        return;
    }

    const SshPlan plan = buildQuickSshPlan(user, host, port);
    if (!plan.valid) {
        QMessageBox::warning(this, tr("Connessione rapida"), plan.error);
        return;
    }
    if (!QFileInfo::exists(plan.program)) {
        QMessageBox::critical(this, tr("OpenSSH non trovato"),
                              tr("Il client \"ssh\" non è stato trovato in %1.")
                                  .arg(plan.program));
        return;
    }

    Connection quick;
    quick.id = ConnectionStore::newId();
    quick.name = normalized;
    quick.host = host;
    quick.port = port;
    quick.user = user;
    quick.auth = AuthType::Agent;

    startSession(quick, plan);
    rememberQuickTarget(normalized);
    statusBar()->showMessage(tr("Connessione rapida a %1").arg(normalized), 5000);
}

void MainWindow::rememberRecentConnection(const QString &id)
{
    if (id.isEmpty())
        return;
    QSettings settings;
    QStringList ids = settings.value(QStringLiteral("palette/recentConnections")).toStringList();
    ids.removeAll(id);
    ids.prepend(id);
    while (ids.size() > 12)
        ids.removeLast();
    settings.setValue(QStringLiteral("palette/recentConnections"), ids);
}

void MainWindow::rememberQuickTarget(const QString &target)
{
    if (target.isEmpty())
        return;
    QSettings settings;
    QStringList targets =
        settings.value(QStringLiteral("palette/recentQuickTargets")).toStringList();
    targets.removeAll(target);
    targets.prepend(target);
    while (targets.size() > 8)
        targets.removeLast();
    settings.setValue(QStringLiteral("palette/recentQuickTargets"), targets);
}

void MainWindow::startSession(const Connection &connection, const SshPlan &plan)
{
    auto *session = new SessionView(connection, plan);
    connect(session, &SessionView::keyPressed, this, &MainWindow::forwardKey);
    connect(session, &SessionView::pasteRequested, this, &MainWindow::forwardPaste);
    connect(session, &SessionView::stateChanged, this, &MainWindow::rebuildSessionList);
    connect(session, &QObject::destroyed, this, [this, session] {
        sessions_.removeAll(session);
        rebuildSessionList();
        updateStatus();
        updateSessionAction();
    });

    QMdiSubWindow *sub = mdi_->addSubWindow(session);
    sub->setAttribute(Qt::WA_DeleteOnClose);
    sub->setWindowTitle(session->title());
    sub->setWindowIcon(style()->standardIcon(QStyle::SP_ComputerIcon));
    sub->resize(940, 580);
    sub->show();

    sessions_.append(session);
    session->setBroadcastActive(broadcastAction_->isChecked());
    session->start();

    rebuildSessionList();
    updateStatus();
    updateSessionAction();
}

void MainWindow::newConnection(const QString &folderId)
{
    if (locked_)
        return;

    QString targetFolder = store_.root.id;
    if (const Node *node = store_.findNode(folderId)) {
        if (node->kind == Node::Kind::Folder)
            targetFolder = node->id;
        else if (Node *parent = store_.parentOf(folderId))
            targetFolder = parent->id;
    }

    ConnectionDialog dialog(store_, this);
    dialog.setFolderId(targetFolder);
    if (dialog.exec() != QDialog::Accepted)
        return;

    Connection connection = dialog.connection();
    connection.id = ConnectionStore::newId();

    Node node;
    node.kind = Node::Kind::Host;
    node.id = connection.id;
    node.name = connection.displayName();
    node.conn = connection;

    Node *folder = store_.findFolder(dialog.folderId());
    if (!folder)
        folder = &store_.root;
    folder->expanded = true;
    folder->children.append(node);

    persist();
    rebuildTree(node.id);
}

void MainWindow::newFolder(const QString &parentId)
{
    if (locked_)
        return;

    QString targetParent = store_.root.id;
    if (const Node *node = store_.findNode(parentId)) {
        if (node->kind == Node::Kind::Folder)
            targetParent = node->id;
        else if (Node *parent = store_.parentOf(parentId))
            targetParent = parent->id;
    }

    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Nuova cartella"), tr("Nome della cartella:"),
                                               QLineEdit::Normal,
                                               tr("Nuovo gruppo %1").arg(++unnamedCounter_), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;

    Node folder;
    folder.kind = Node::Kind::Folder;
    folder.id = ConnectionStore::newId();
    folder.name = name.trimmed();

    Node *parent = store_.findFolder(targetParent);
    if (!parent)
        parent = &store_.root;
    parent->expanded = true;
    parent->children.append(folder);

    persist();
    rebuildTree(folder.id);
}

void MainWindow::editConnection(const QString &id)
{
    if (locked_)
        return;
    const Connection *existing = store_.findConnection(id);
    if (!existing)
        return;

    Node *parent = store_.parentOf(id);
    ConnectionDialog dialog(store_, this);
    dialog.setConnection(*existing);
    dialog.setFolderId(parent ? parent->id : store_.root.id);
    if (dialog.exec() != QDialog::Accepted)
        return;

    Connection updated = dialog.connection();
    updated.id = id;

    Node node;
    node.kind = Node::Kind::Host;
    node.id = id;
    node.name = updated.displayName();
    node.conn = updated;

    Node removed;
    if (removeNodeRecursive(store_.root, id, &removed))
        removed.conn.clearSensitive();

    Node *target = store_.findFolder(dialog.folderId());
    if (!target)
        target = &store_.root;
    target->expanded = true;
    target->children.append(node);

    persist();
    rebuildTree(id);
}

void MainWindow::renameNode(const QString &id)
{
    if (locked_)
        return;
    Node *node = store_.findNode(id);
    if (!node || node == &store_.root)
        return;

    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rinomina"), tr("Nuovo nome:"),
                                               QLineEdit::Normal, node->name, &ok);
    if (!ok || name.trimmed().isEmpty())
        return;

    node->name = name.trimmed();
    if (node->kind == Node::Kind::Host)
        node->conn.name = node->name;

    persist();
    rebuildTree(id);
}

void MainWindow::deleteNode(const QString &id)
{
    if (locked_)
        return;
    Node *node = store_.findNode(id);
    if (!node || node == &store_.root)
        return;

    const QString label = node->name.isEmpty() ? node->conn.displayName() : node->name;
    const QString question =
        node->kind == Node::Kind::Folder
            ? tr("Eliminare la cartella \"%1\" con tutto il suo contenuto?").arg(label)
            : tr("Eliminare la connessione \"%1\"?").arg(label);

    if (QMessageBox::question(this, tr("Conferma eliminazione"), question,
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes)
        return;

    Node removed;
    if (removeNodeRecursive(store_.root, id, &removed))
        wipeSecrets(removed);

    persist();
    rebuildTree();
    updateSessionAction();
}

void MainWindow::duplicateConnection(const QString &id)
{
    if (locked_)
        return;
    Node *node = store_.findNode(id);
    Node *parent = store_.parentOf(id);
    if (!node || node->kind != Node::Kind::Host || !parent)
        return;

    Node copy = *node;
    copy.id = ConnectionStore::newId();
    copy.conn.id = copy.id;
    copy.conn.name = node->conn.displayName() + tr(" (copia)");
    copy.name = copy.conn.name;

    for (int i = 0; i < parent->children.size(); ++i) {
        if (parent->children.at(i).id == id) {
            parent->children.insert(i + 1, copy);
            break;
        }
    }

    persist();
    rebuildTree(copy.id);
}

void MainWindow::forwardKey(SessionView *from, QKeyEvent *event)
{
    if (locked_ || !broadcastAction_->isChecked())
        return;
    for (SessionView *session : sessions_) {
        if (session != from && session->isIncludedInBroadcast())
            session->sendKeyEvent(event);
    }
}

void MainWindow::forwardPaste(SessionView *from, const QString &text)
{
    if (locked_ || !broadcastAction_->isChecked())
        return;
    for (SessionView *session : sessions_) {
        if (session != from && session->isIncludedInBroadcast())
            session->sendText(text);
    }
}

void MainWindow::updateBroadcastState()
{
    const bool active = broadcastAction_->isChecked();
    for (SessionView *session : sessions_)
        session->setBroadcastActive(active);
    rebuildSessionList();
    updateStatus();
}

void MainWindow::showAbout()
{
    QMessageBox::about(
        this, tr("Informazioni su SSH Manager"),
        tr("<h3>SSH Manager</h3>"
           "<p>Gestore di sessioni SSH con finestre MDI, costruito con Qt %1 e QTermWidget.</p>"
           "<p>Connessioni e chiavi private sono raccolte in un database cifrato:<br><code>%2</code>"
           "<br>protetto da master password (AES-256-GCM, chiave PBKDF2). "
           "Il blocco automatico è impostabile dal menu Sicurezza.</p>"
           "<p>Le sessioni usano il client OpenSSH di sistema: chiavi, agent e configurazione "
           "standard funzionano come da terminale.</p>")
            .arg(QString::fromLatin1(qVersion()), vault_.path()));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    persist();

    const auto windows = mdi_->subWindowList();
    for (QMdiSubWindow *sub : windows)
        sub->close();

    store_.clearSensitive();
    vault_.lock();

    QMainWindow::closeEvent(event);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (!locked_) {
        switch (event->type()) {
        case QEvent::KeyPress:
        case QEvent::MouseButtonPress:
        case QEvent::Wheel:
        case QEvent::TouchBegin:
            armAutoLock();
            break;
        default:
            break;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}
