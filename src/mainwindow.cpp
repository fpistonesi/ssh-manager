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
#include "apptheme.h"
#include "archivetransfer.h"
#include "icons.h"
#include "openssh.h"
#include "remoterun.h"
#include "batchdialog.h"
#include "hosthealth.h"
#include "knownhosts.h"
#include "knownhostsdialog.h"
#include "snippetdialog.h"
#include "snippets.h"

#include <QFontDatabase>
#include "tunnelprobe.h"
#include "tunnelservice.h"

#include <QCollator>
#include <QEventLoop>
#include <QPlainTextEdit>
#include <QProcess>
#include <QSaveFile>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QStyleHints>
#include <QFormLayout>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QUrl>
#include <QDir>
#include <QDockWidget>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
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
#include <QSystemTrayIcon>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <qtermwidget.h>

#include <algorithm>
#include <cmath>
#include <functional>

namespace {

// Columns of the connection tree; favourite and tags are optional.
enum TreeColumn { TreeNameColumn, TreeTargetColumn, TreeFavoriteColumn, TreeTagsColumn, TreeColumnCount };

// Folders first, then a natural, case-insensitive order ("web2" < "web10").
bool nameLessThan(bool aFolder, const QString &a, bool bFolder, const QString &b)
{
    if (aFolder != bFolder)
        return aFolder;
    static const QCollator collator = [] {
        QCollator c;
        c.setNumericMode(true);
        c.setCaseSensitivity(Qt::CaseInsensitive);
        return c;
    }();
    return collator.compare(a, b) < 0;
}

bool nodeLessThan(const Node &a, const Node &b)
{
    const QString nameA = a.name.isEmpty() ? a.conn.displayName() : a.name;
    const QString nameB = b.name.isEmpty() ? b.conn.displayName() : b.name;
    return nameLessThan(a.kind == Node::Kind::Folder, nameA, b.kind == Node::Kind::Folder, nameB);
}

// Tree item sorted by nameLessThan() when the view is alphabetical.
class NodeItem : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem &other) const override
    {
        const auto folder = [](const QTreeWidgetItem &item) {
            return item.data(0, Qt::UserRole + 1).toInt() == int(Node::Kind::Folder);
        };
        return nameLessThan(folder(*this), text(0), folder(other), other.text(0));
    }
};

// Tunnel panel layout and the item data roles of its rows.
enum TunnelColumn {
    TunnelTypeColumn,
    TunnelLocalColumn,
    TunnelRemoteColumn,
    TunnelConnectionColumn,
    TunnelStateColumn,
    TunnelColumnCount
};
enum TunnelRole {
    TunnelOwnerRole = Qt::UserRole, // SessionView* or TunnelService*
    TunnelKindRole,                 // TunnelFromSession / TunnelFromService
    TunnelTypeRole,                 // TunnelType
    TunnelPortRole,                 // local port
    TunnelAddressRole,              // "host:port" shown in the Local column
};
enum TunnelOrigin { TunnelFromSession, TunnelFromService };

} // namespace

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
    if (paths::isPortable()) {
        auto *portable = new QLabel(tr("Portabile"), this);
        portable->setToolTip(tr("Archivio, chiavi host e impostazioni in %1")
                                 .arg(QDir::toNativeSeparators(paths::portableDir())));
        statusBar()->addPermanentWidget(portable);
    }

    createActions();
    createDocks();
    if (tabbedView_)
        setTabbedView(true);

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

    applyAppStyle();
    // Light/dark switches (from the system in "system" mode, or from
    // Visualizza > Aspetto): the whole application theme is re-applied, then
    // everything drawn with theme colours is redrawn.
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        apptheme::refresh();
        refreshThemeDependentUi();
    });

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
        // Hidden in the tray: ask for the password when the window is shown
        // again, not with a dialog popping up out of nowhere.
        if (isVisible())
            QTimer::singleShot(0, this, &MainWindow::requestUnlock);
    });

    createTray();

    qApp->installEventFilter(this);

    connect(mdi_, &QMdiArea::subWindowActivated, this, [this](QMdiSubWindow *) {
        const SessionView *current = activeSession();
        for (SessionView *session : std::as_const(sessions_))
            session->setActive(session == current);
        updateSessionAction();
        highlightActiveSession();
        rebuildHistoryList();
        if (!locked_) {
            if (SessionView *session = activeSession())
                session->terminal()->setFocus();
        }
    });

    rebuildTree();
    applyLockedState();
    updateStatus();

    // Window size and position come back before the first show; the dock
    // layout after it (see showEvent).
    const QByteArray geometry = QSettings().value(QStringLiteral("window/geometry")).toByteArray();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);

    if (QSettings().value(QStringLiteral("view/reachability"), false).toBool())
        setReachabilityEnabled(true);

    QTimer::singleShot(0, this, &MainWindow::initializeVault);
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    if (defaultLayoutCaptured_)
        return;
    defaultLayoutCaptured_ = true;
    // The layout as built in code, laid out at the real window size: what
    // "Ripristina layout" goes back to.
    defaultLayout_ = saveState(kLayoutVersion);
    const QByteArray saved = QSettings().value(QStringLiteral("window/state")).toByteArray();
    if (!saved.isEmpty())
        restoreState(saved, kLayoutVersion);
}

void MainWindow::saveLayout()
{
    if (!defaultLayoutCaptured_)
        return;
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("window/state"), saveState(kLayoutVersion));
}

void MainWindow::resetLayout()
{
    if (defaultLayout_.isEmpty())
        return;
    restoreState(defaultLayout_, kLayoutVersion);
    saveLayout();
    statusBar()->showMessage(tr("Layout dei pannelli ripristinato"), 4000);
}

void MainWindow::applyAppStyle()
{
    // The look comes from the application-wide theme (apptheme); the MDI
    // area paints its own background brush, which no style sheet reaches.
    if (mdi_)
        mdi_->setBackground(QApplication::palette().window());
}

void MainWindow::refreshThemeDependentUi()
{
    applyAppStyle();
    refreshThemedIcons();
    for (SessionView *session : std::as_const(sessions_))
        session->applyTerminalProfile(session->connection().terminal);
    // Lists with status colours are rebuilt with the new ones.
    for (QTreeWidgetItem *item : std::as_const(itemById_))
        applyReachability(item);
    rebuildSessionList();
    rebuildTunnelList();
    updateTray();
    if (filterButton_)
        updateFilterIndicator(); // its marked icon is a pixmap in the old colours
}

QIcon MainWindow::layoutIcon(int kind) const
{
    // Layout icons are drawn explicitly: theme icons for these operations are
    // often missing or indistinguishable.
    QPixmap pixmap(48, 48);
    pixmap.fill(Qt::transparent);
    pixmap.setDevicePixelRatio(2);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true); // 24x24 logical units (DPR 2)
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
    } else if (kind == 5) { // tabs: a tab strip over one pane
        QColor back = color;
        back.setAlphaF(0.30);
        painter.setBrush(back);
        painter.drawRoundedRect(QRectF(10, 3.5, 5.5, 4), 1, 1);
        painter.drawRoundedRect(QRectF(16.5, 3.5, 4.5, 4), 1, 1);
        painter.setBrush(color);
        painter.drawRoundedRect(QRectF(3, 3.5, 6, 4), 1, 1);
        painter.drawRoundedRect(QRectF(3, 8, 18, 12.5), r, r);
    } else if (kind == 6) { // theme: a half-filled circle
        QPen pen(color, 1.6);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QRectF(4, 4, 16, 16));
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawPie(QRectF(4, 4, 16, 16), 90 * 16, 180 * 16);
    } else if (kind == 7) { // language: a globe
        QPen pen(color, 1.4);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QRectF(3.5, 3.5, 17, 17));
        painter.drawEllipse(QRectF(8, 3.5, 8, 17));
        painter.drawLine(QPointF(3.5, 12), QPointF(20.5, 12));
        painter.drawLine(QPointF(5.5, 7.5), QPointF(18.5, 7.5));
        painter.drawLine(QPointF(5.5, 16.5), QPointF(18.5, 16.5));
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
}

void MainWindow::refreshThemedIcons()
{
    tileAction_->setIcon(layoutIcon(0));
    tileVerticalAction_->setIcon(layoutIcon(1));
    tileHorizontalAction_->setIcon(layoutIcon(2));
    gridAction_->setIcon(layoutIcon(3));
    cascadeAction_->setIcon(layoutIcon(4));
    tabbedViewAction_->setIcon(layoutIcon(5));
    if (themeButton_)
        themeButton_->setIcon(icons::get(QStringLiteral("theme")));
    if (languageButton_)
        languageButton_->setIcon(icons::get(QStringLiteral("language")));
}

void MainWindow::setLanguage(const QString &language)
{
    // The effective language of a setting: "" follows the system.
    const auto effective = [](const QString &value) {
        if (value == QLatin1String("it") || value == QLatin1String("en"))
            return value;
        return QLocale::system().language() == QLocale::Italian ? QStringLiteral("it")
                                                                : QStringLiteral("en");
    };
    QSettings settings;
    const QString before = effective(settings.value(QStringLiteral("ui/language")).toString());
    settings.setValue(QStringLiteral("ui/language"), language);
    for (QAction *action : languageActions_)
        action->setChecked(action->data().toString() == language);
    if (effective(language) != before) {
        QMessageBox::information(this, tr("Lingua"),
                                 tr("La nuova lingua sarà applicata al prossimo avvio di SSH Manager."));
    }
}

void MainWindow::setTheme(const QString &theme)
{
    QSettings().setValue(QStringLiteral("view/theme"), theme);
    apptheme::setMode(theme);
    refreshThemeDependentUi();
    for (QAction *action : themeActions_)
        action->setChecked(action->data().toString() == theme);
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
    tabbedView_ = settings.value(QStringLiteral("view/tabbed"), false).toBool();
    keepInTray_ = settings.value(QStringLiteral("view/keepInTray"), true).toBool();
    sortByName_ = settings.value(QStringLiteral("view/sortByName"), false).toBool();
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

    exportAction_ = new QAction(tr("Esporta connessioni…"), this);
    exportAction_->setIconText(tr("Esporta"));
    exportAction_->setToolTip(tr("Esporta in un file cifrato le connessioni scelte (quelle "
                                 "selezionate o tutte), con o senza credenziali"));
    connect(exportAction_, &QAction::triggered, this, &MainWindow::exportArchive);

    importAction_ = new QAction(tr("Importa connessioni…"), this);
    importAction_->setIconText(tr("Importa"));
    importAction_->setToolTip(tr("Aggiunge connessioni da un file esportato, o sostituisce "
                                 "l'intero archivio"));
    connect(importAction_, &QAction::triggered, this, &MainWindow::importArchive);

    importSshConfigAction_ = new QAction(tr("Importa da ~/.ssh/config…"), this);
    importSshConfigAction_->setIconText(tr("Da OpenSSH"));
    importSshConfigAction_->setToolTip(tr("Crea connessioni dagli host del file di configurazione "
                                          "di OpenSSH (HostName, User, Port, IdentityFile, ProxyJump)"));
    connect(importSshConfigAction_, &QAction::triggered, this, &MainWindow::importSshConfig);

    exportSshConfigAction_ = new QAction(tr("Esporta per ~/.ssh/config…"), this);
    exportSshConfigAction_->setIconText(tr("Per OpenSSH"));
    exportSshConfigAction_->setToolTip(tr("Blocchi Host per il file di configurazione di OpenSSH, "
                                          "dalle connessioni selezionate o da tutte"));
    connect(exportSshConfigAction_, &QAction::triggered, this, &MainWindow::exportSshConfig);

    generateKeyAction_ = new QAction(tr("Genera chiave SSH…"), this);
    generateKeyAction_->setIconText(tr("Genera chiave"));
    generateKeyAction_->setToolTip(tr("Crea una coppia di chiavi con ssh-keygen; dalla "
                                      "connessione selezionata può anche installarla sull'host"));
    connect(generateKeyAction_, &QAction::triggered, this, [this] { generateKey(selectedId()); });

    copyKeyAction_ = new QAction(tr("Copia la chiave pubblica sull'host…"), this);
    copyKeyAction_->setIconText(tr("Copia chiave sull'host"));
    copyKeyAction_->setToolTip(tr("Aggiunge una chiave pubblica ad authorized_keys dell'host "
                                  "selezionato, come ssh-copy-id"));
    connect(copyKeyAction_, &QAction::triggered, this, [this] { copyPublicKey(selectedId()); });

    backupAction_ = new QAction(tr("Backup archivio…"), this);
    connect(backupAction_, &QAction::triggered, this, &MainWindow::backupArchive);

    quitAction_ = new QAction(tr("Esci"), this);
    quitAction_->setShortcut(QKeySequence::Quit);
    connect(quitAction_, &QAction::triggered, this, &MainWindow::quitApplication);

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
    transferAction_->setIconText(tr("Trasferisci file"));
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

    startTunnelsAction_ = new QAction(tr("Avvia tunnel in background"), this);
    startTunnelsAction_->setIconText(tr("Avvia tunnel"));
    startTunnelsAction_->setToolTip(
        tr("Avvia i tunnel della connessione selezionata senza aprire un terminale, "
           "con riconnessione automatica"));
    connect(startTunnelsAction_, &QAction::triggered, this,
            [this] { startTunnelService(selectedId()); });

    probe_ = new HostProbe(this);
    connect(probe_, &HostProbe::result, this,
            [this](const QString &key, const HostProbe::Result &result) {
                if (locked_)
                    return;
                reachability_.insert(key, result);
                applyReachability(itemById_.value(key));
            });
    reachabilityTimer_ = new QTimer(this);
    reachabilityTimer_->setInterval(kReachabilityMinutes * 60 * 1000);
    connect(reachabilityTimer_, &QTimer::timeout, this, [this] { checkReachability(); });

    reachabilityAction_ = new QAction(tr("Raggiungibilità automatica"), this);
    reachabilityAction_->setIconText(tr("Raggiungibilità"));
    reachabilityAction_->setToolTip(
        tr("Verifica periodicamente se gli host rispondono e mostra un indicatore nell'albero"));
    reachabilityAction_->setCheckable(true);
    connect(reachabilityAction_, &QAction::toggled, this, &MainWindow::setReachabilityEnabled);

    checkReachabilityAction_ = new QAction(tr("Verifica raggiungibilità ora"), this);
    checkReachabilityAction_->setIconText(tr("Verifica ora"));
    checkReachabilityAction_->setToolTip(tr("Verifica subito tutti gli host salvati"));
    connect(checkReachabilityAction_, &QAction::triggered, this, [this] { checkReachability(); });

    batchAction_ = new QAction(tr("Esegui su più host…"), this);
    batchAction_->setIconText(tr("Esegui su più host"));
    batchAction_->setToolTip(tr("Esegui un comando sugli host selezionati e raccogli l'esito di ciascuno"));
    batchAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+E")));
    connect(batchAction_, &QAction::triggered, this, [this] { runBatch(); });

    healthAction_ = new QAction(tr("Stato dell'host…"), this);
    healthAction_->setIconText(tr("Stato host"));
    healthAction_->setToolTip(tr("Carico, memoria, dischi, servizi in errore e processi dell'host selezionato"));
    connect(healthAction_, &QAction::triggered, this, [this] { showHostHealth(selectedId()); });

    externalToolsAction_ = new QAction(tr("Strumenti esterni…"), this);
    externalToolsAction_->setIconText(tr("Strumenti esterni"));
    externalToolsAction_->setToolTip(
        tr("Programmi da avviare su una connessione (ping, tracert, ftp…), dal menu del tasto destro"));
    connect(externalToolsAction_, &QAction::triggered, this, [this] {
        ExternalToolsDialog dialog(this);
        dialog.exec();
    });

    // Theme: system / light / dark.
    auto *themeGroup = new QActionGroup(this);
    themeGroup->setExclusive(true);
    const QString currentTheme = QSettings().value(QStringLiteral("view/theme"), QStringLiteral("system")).toString();
    const QList<QPair<QString, QString>> themes = {
        {QStringLiteral("system"), tr("Tema di sistema")},
        {QStringLiteral("light"), tr("Tema chiaro")},
        {QStringLiteral("dark"), tr("Tema scuro")},
    };
    for (const auto &theme : themes) {
        auto *action = new QAction(theme.second, this);
        action->setCheckable(true);
        action->setData(theme.first);
        action->setChecked(theme.first == currentTheme);
        themeGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, id = theme.first] { setTheme(id); });
        themeActions_ << action;
    }

    // Language: system / Italian / English, applied at the next start. The
    // language names stay in their own language.
    auto *languageGroup = new QActionGroup(this);
    languageGroup->setExclusive(true);
    QString currentLanguage = QSettings().value(QStringLiteral("ui/language")).toString();
    if (currentLanguage != QLatin1String("it") && currentLanguage != QLatin1String("en"))
        currentLanguage.clear();
    const QList<QPair<QString, QString>> languages = {
        {QString(), tr("Lingua di sistema")},
        {QStringLiteral("it"), QStringLiteral("Italiano")},
        {QStringLiteral("en"), QStringLiteral("English")},
    };
    for (const auto &language : languages) {
        auto *action = new QAction(language.second, this);
        action->setCheckable(true);
        action->setData(language.first);
        action->setChecked(language.first == currentLanguage);
        languageGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, id = language.first] { setLanguage(id); });
        languageActions_ << action;
    }

    resetLayoutAction_ = new QAction(tr("Ripristina layout predefinito"), this);
    resetLayoutAction_->setIconText(tr("Ripristina layout"));
    resetLayoutAction_->setToolTip(
        tr("Riporta pannelli e schede nella posizione e dimensione iniziali"));
    connect(resetLayoutAction_, &QAction::triggered, this, &MainWindow::resetLayout);

    newSnippetAction_ = new QAction(tr("Nuovo snippet…"), this);
    newSnippetAction_->setIconText(tr("Nuovo snippet"));
    newSnippetAction_->setToolTip(tr("Salva un comando da riusare, con variabili"));
    connect(newSnippetAction_, &QAction::triggered, this, [this] { newSnippet(Snippet()); });

    knownHostsAction_ = new QAction(tr("Chiavi host…"), this);
    knownHostsAction_->setIconText(tr("Chiavi host"));
    knownHostsAction_->setToolTip(
        tr("Elenco delle chiavi dei server accettate, con fingerprint e data del primo contatto"));
    connect(knownHostsAction_, &QAction::triggered, this, [this] { showKnownHosts(); });

    tabbedViewAction_ = new QAction(tr("Vista a schede"), this);
    tabbedViewAction_->setIconText(tr("Schede"));
    tabbedViewAction_->setToolTip(tr("Mostra le sessioni come schede invece che come finestre"));
    tabbedViewAction_->setCheckable(true);
    tabbedViewAction_->setChecked(tabbedView_);
    connect(tabbedViewAction_, &QAction::toggled, this, &MainWindow::setTabbedView);

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

    // The application's own line icons: they follow the light/dark theme.
    const QList<QPair<QAction *, const char *>> actionIcons = {
        {newConnectionAction_, "server-add"},   {newFolderAction_, "folder-add"},
        {editAction_, "edit"},                  {duplicateAction_, "copy"},
        {renameAction_, "rename"},              {deleteAction_, "delete"},
        {connectAction_, "connect"},            {localTerminalAction_, "local-terminal"},
        {quickAction_, "search"},               {saveAction_, "save"},
        {exportAction_, "export"},              {importAction_, "import"},
        {backupAction_, "backup"},              {unlockAction_, "unlock"},
        {lockAction_, "lock"},                  {changePasswordAction_, "password"},
        {broadcastAction_, "broadcast"},        {includeAction_, "include"},
        {terminalAction_, "terminal-settings"}, {transferAction_, "transfer"},
        {startTunnelsAction_, "tunnel"},        {knownHostsAction_, "host-keys"},
        {newSnippetAction_, "snippet-add"},     {resetLayoutAction_, "reset-layout"},
        {reachabilityAction_, "reachability"},  {batchAction_, "batch"},
        {healthAction_, "health"},              {checkReachabilityAction_, "refresh"},
        {favoriteAction_, "star"},              {favoritesOnlyAction_, "star"},
        {closeSessionAction_, "close"},         {closeAllAction_, "close-all"},
        {maximizeSessionAction_, "maximize"},   {restoreSessionAction_, "restore"},
        {previousSessionAction_, "previous"},   {nextSessionAction_, "next"},
        {externalToolsAction_, "tools"},        {importSshConfigAction_, "file-import"},
        {exportSshConfigAction_, "file-export"}, {generateKeyAction_, "key-add"},
        {copyKeyAction_, "key-send"},
    };
    for (const auto &entry : actionIcons) {
        if (entry.first)
            entry.first->setIcon(icons::get(QString::fromLatin1(entry.second)));
    }
    refreshThemedIcons();

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
        startTunnelsAction_,     knownHostsAction_,     newSnippetAction_,
        batchAction_,            healthAction_,
        closeSessionAction_,     closeAllAction_,
        tileAction_,             tileVerticalAction_,
        tileHorizontalAction_,   gridAction_,           cascadeAction_,
        tabbedViewAction_,       maximizeSessionAction_,  restoreSessionAction_, previousSessionAction_,
        nextSessionAction_,
    };
    for (QAction *action : globalActions)
        addAction(action);

    aboutAction_ = new QAction(tr("Informazioni"), this);
    aboutAction_->setIcon(icons::get(QStringLiteral("info")));
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

    // Compact variant: small icon beside the label, so two of them stack in
    // the height of one large button.
    auto makeSmallButton = [](QAction *action, QWidget *parentWidget) {
        auto *button = new QToolButton(parentWidget);
        button->setDefaultAction(action);
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setIconSize(QSize(16, 16));
        button->setAutoRaise(true);
        button->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        return button;
    };

    // Lays small items out column by column, two per column, top aligned.
    auto makeStackOf = [](const QList<QWidget *> &items, QWidget *parentWidget) {
        auto *stack = new QWidget(parentWidget);
        auto *grid = new QGridLayout(stack);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(2);
        grid->setVerticalSpacing(0);
        for (int i = 0; i < items.size(); ++i) {
            items.at(i)->setParent(stack);
            grid->addWidget(items.at(i), i % 2, i / 2, Qt::AlignLeft | Qt::AlignTop);
        }
        grid->setRowStretch(2, 1);
        return stack;
    };
    auto makeStack = [&](const QList<QAction *> &actions, QWidget *parentWidget) {
        QList<QWidget *> buttons;
        for (QAction *action : actions)
            buttons << makeSmallButton(action, parentWidget);
        return makeStackOf(buttons, parentWidget);
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

    // Small button with the auto-lock delay menu.
    auto makeAutoLockButton = [this](QWidget *parentWidget) {
        auto *button = new QToolButton(parentWidget);
        button->setText(tr("Blocco automatico"));
        button->setToolTip(tr("Blocca l'archivio dopo un periodo di inattività"));
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setIcon(icons::get(QStringLiteral("timer")));
        button->setIconSize(QSize(16, 16));
        button->setAutoRaise(true);
        button->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
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

    // Large button that opens a menu of exclusive choices.
    auto makeDropdown = [&](const QString &text, const QString &toolTip,
                            const QList<QAction *> &actions, QWidget *parentWidget) {
        auto *button = new QToolButton(parentWidget);
        button->setText(text);
        button->setToolTip(toolTip);
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setIconSize(QSize(20, 20));
        button->setAutoRaise(true);
        button->setPopupMode(QToolButton::InstantPopup);
        auto *menu = new QMenu(button);
        menu->addActions(actions);
        button->setMenu(menu);
        button->setMinimumWidth(qMax(64, button->sizeHint().width()));
        button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        return button;
    };
    themeButton_ = makeDropdown(tr("Tema"), tr("Tema dell'applicazione: di sistema, chiaro o scuro"),
                                themeActions_, tabs);
    languageButton_ = makeDropdown(tr("Lingua"), tr("Lingua dell'interfaccia (al prossimo avvio)"),
                                   languageActions_, tabs);
    refreshThemedIcons();

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

    // Frequent actions are large buttons; occasional ones are small, stacked
    // two per column.
    addTab(tr("Connessioni"),
           {makeGroup(tr("Connetti"),
                      {makeButton(connectAction_, tabs), makeButton(quickAction_, tabs),
                       makeStack({localTerminalAction_, transferAction_}, tabs)},
                      tabs),
            makeGroup(tr("Tunnel"), {makeButton(startTunnelsAction_, tabs)}, tabs),
            makeGroup(tr("Strumenti"),
                      {makeStack({batchAction_, healthAction_}, tabs)}, tabs),
            makeGroup(tr("Stato"),
                      {makeStack({reachabilityAction_, checkReachabilityAction_}, tabs)}, tabs),
            makeGroup(tr("Gestione"),
                      {makeButton(newConnectionAction_, tabs),
                       makeStack({newFolderAction_, editAction_, duplicateAction_, renameAction_,
                                  favoriteAction_, deleteAction_},
                                 tabs)},
                      tabs)});

    addTab(tr("Sessioni"),
           {makeGroup(tr("Input"),
                      {makeButton(broadcastAction_, tabs), makeStack({includeAction_}, tabs)},
                      tabs),
            makeGroup(tr("Terminale"), {makeButton(terminalAction_, tabs)}, tabs),
            makeGroup(tr("Comandi"),
                      {makeButton(newSnippetAction_, tabs),
                       makeStack({snippetDock_->toggleViewAction(),
                                  historyDock_->toggleViewAction()},
                                 tabs)},
                      tabs),
            makeGroup(tr("Navigazione"),
                      {makeStack({previousSessionAction_, nextSessionAction_,
                                  closeSessionAction_, closeAllAction_},
                                 tabs)},
                      tabs)});

    addTab(tr("Visualizza"),
           {makeGroup(tr("Disposizione"),
                      {makeButton(tabbedViewAction_, tabs),
                       makeStack({tileAction_, gridAction_, tileVerticalAction_,
                                  tileHorizontalAction_, cascadeAction_},
                                 tabs)},
                      tabs),
            makeGroup(tr("Aspetto"), {themeButton_, languageButton_}, tabs),
            makeGroup(tr("Finestra attiva"),
                      {makeStack({maximizeSessionAction_, restoreSessionAction_}, tabs)}, tabs),
            makeGroup(tr("Pannelli"),
                      {makeStack({connectionsDock_->toggleViewAction(),
                                  sessionDock_->toggleViewAction(),
                                  tunnelDock_->toggleViewAction(),
                                  snippetDock_->toggleViewAction(),
                                  historyDock_->toggleViewAction(), resetLayoutAction_},
                                 tabs)},
                      tabs)});

    addTab(tr("Archivio"),
           {makeGroup(tr("Dati"),
                      {makeButton(saveAction_, tabs),
                       makeStack({exportAction_, importAction_, backupAction_}, tabs)},
                      tabs),
            makeGroup(tr("OpenSSH"),
                      {makeStack({importSshConfigAction_, exportSshConfigAction_,
                                  generateKeyAction_, copyKeyAction_},
                                 tabs)},
                      tabs),
            makeGroup(tr("Sicurezza"),
                      {makeButton(lockAction_, tabs),
                       makeStackOf({makeSmallButton(unlockAction_, tabs),
                                    makeSmallButton(changePasswordAction_, tabs),
                                    makeAutoLockButton(tabs),
                                    makeSmallButton(knownHostsAction_, tabs)},
                                   tabs)},
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

void MainWindow::setTabbedView(bool tabbed)
{
    tabbedView_ = tabbed;
    QSettings().setValue(QStringLiteral("view/tabbed"), tabbed);

    if (tabbed) {
        // A tab shows its window maximized: QMdiArea does that on activation
        // only without DontMaximizeSubWindowOnActivation, and windows that
        // are already open must be maximized here.
        mdi_->setOption(QMdiArea::DontMaximizeSubWindowOnActivation, false);
        mdi_->setViewMode(QMdiArea::TabbedView);
        QMdiSubWindow *active = mdi_->activeSubWindow();
        const auto windows = mdi_->subWindowList();
        for (QMdiSubWindow *sub : windows)
            sub->showMaximized();
        if (active)
            mdi_->setActiveSubWindow(active);
        mdi_->setDocumentMode(true);
        mdi_->setTabsClosable(true);
        mdi_->setTabsMovable(true);
        // The tab bar exists only once the view mode is tabbed.
        if (auto *bar = mdi_->findChild<QTabBar *>()) {
            bar->setExpanding(false);
            bar->setElideMode(Qt::ElideMiddle);
            bar->setUsesScrollButtons(true);
        }
    } else {
        mdi_->setOption(QMdiArea::DontMaximizeSubWindowOnActivation, true);
        mdi_->setViewMode(QMdiArea::SubWindowView);
        // Tabs leave every window maximized: lay them out side by side again.
        const auto windows = mdi_->subWindowList();
        for (QMdiSubWindow *sub : windows)
            sub->showNormal();
        if (!windows.isEmpty())
            mdi_->tileSubWindows();
    }

    {
        const QSignalBlocker blocker(tabbedViewAction_);
        tabbedViewAction_->setChecked(tabbed);
    }
    updateSessionAction();
}

void MainWindow::cycleSession(int direction)
{
    // In tabbed view follow the tab order (QMdiArea keeps its creation order
    // in sync with moved tabs), otherwise the window stacking order.
    const auto windows = mdi_->subWindowList(tabbedView_ ? QMdiArea::CreationOrder
                                                         : QMdiArea::StackingOrder);
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
    filterIcon_ = icons::get(QStringLiteral("filter"));
    filterButton->setIcon(filterIcon_);
    filterButton_ = filterButton;

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

    // Optional columns, hidden by default.
    filterMenu->addSeparator();
    QMenu *columnsMenu = filterMenu->addMenu(tr("Colonne"));
    const QList<QPair<int, QString>> optionalColumns = {
        {TreeTargetColumn, tr("Destinazione")},
        {TreeFavoriteColumn, tr("Preferito (★)")},
        {TreeTagsColumn, tr("Tag")},
    };
    for (const auto &column : optionalColumns) {
        QAction *action = columnsMenu->addAction(column.second);
        action->setCheckable(true);
        connect(action, &QAction::toggled, this,
                [this, index = column.first](bool on) { setExtraColumnVisible(index, on); });
    }
    connect(columnsMenu, &QMenu::aboutToShow, this, [this, columnsMenu, optionalColumns] {
        const QList<QAction *> actions = columnsMenu->actions();
        for (int i = 0; i < actions.size() && i < optionalColumns.size(); ++i) {
            const QSignalBlocker blocker(actions.at(i));
            actions.at(i)->setChecked(!tree_->isColumnHidden(optionalColumns.at(i).first));
        }
    });

    // Order of the tree: alphabetical view, or the stored (manual) order,
    // which drag and drop changes.
    QMenu *sortMenu = filterMenu->addMenu(tr("Ordinamento"));
    auto *sortGroup = new QActionGroup(sortMenu);
    QAction *sortManual = sortMenu->addAction(tr("Manuale (trascina per spostare)"));
    QAction *sortName = sortMenu->addAction(tr("Alfabetico"));
    for (QAction *action : {sortManual, sortName}) {
        action->setCheckable(true);
        sortGroup->addAction(action);
    }
    (sortByName_ ? sortName : sortManual)->setChecked(true);
    connect(sortManual, &QAction::triggered, this, [this] { setSortByName(false); });
    connect(sortName, &QAction::triggered, this, [this] { setSortByName(true); });
    sortMenu->addSeparator();
    QAction *sortStore = sortMenu->addAction(tr("Riordina alfabeticamente l'archivio"));
    sortStore->setToolTip(tr("Salva l'ordine alfabetico come ordine manuale, da ritoccare poi a mano"));
    connect(sortStore, &QAction::triggered, this, &MainWindow::sortStoreByName);
    filterButton->setMenu(filterMenu);

    auto *filterRow = new QHBoxLayout();
    filterRow->setContentsMargins(0, 0, 0, 0);
    filterRow->setSpacing(4);
    filterRow->addWidget(filter_, 1);
    filterRow->addWidget(filterButton);

    tree_ = new QTreeWidget(container);
    tree_->setColumnCount(TreeColumnCount);
    tree_->setHeaderLabels({tr("Nome"), tr("Destinazione"), QStringLiteral("★"), tr("Tag")});
    tree_->headerItem()->setToolTip(TreeFavoriteColumn, tr("Preferito"));
    tree_->headerItem()->setTextAlignment(TreeFavoriteColumn, Qt::AlignCenter);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    // Several hosts (Ctrl/Shift+click) for "run on several hosts"; the other
    // actions keep working on the current item.
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    // Drag and drop: reorder, or move into another folder (synced to the
    // store after each drop, see eventFilter()).
    tree_->setDragEnabled(true);
    tree_->setAcceptDrops(true);
    tree_->setDropIndicatorShown(true);
    tree_->setDragDropMode(QAbstractItemView::InternalMove);
    tree_->setDefaultDropAction(Qt::MoveAction);
    tree_->setAlternatingRowColors(true);
    tree_->setUniformRowHeights(true);
    tree_->setAnimated(true);
    // "connections2": the header gained the favourite and tag columns.
    setupResizableHeader(tree_, QStringLiteral("connections2"), 190);
    if (!QSettings().contains(QStringLiteral("view/connections2Header")))
        tree_->header()->resizeSection(TreeFavoriteColumn, 40);
    // Destination shown by default, favourite and tags hidden.
    for (int column : {int(TreeTargetColumn), int(TreeFavoriteColumn), int(TreeTagsColumn)}) {
        const QString key = QStringLiteral("view/connectionsColumn%1").arg(column);
        const bool byDefault = column == TreeTargetColumn;
        tree_->setColumnHidden(column, !QSettings().value(key, byDefault).toBool());
    }
    tree_->setRootIsDecorated(true);

    layout->addLayout(filterRow);
    layout->addWidget(tree_, 1);
    dock->setWidget(container);
    dock->setMinimumWidth(300);
    addDockWidget(Qt::LeftDockWidgetArea, dock);
    connectionsDock_ = dock;
    dock->toggleViewAction()->setIcon(
        icons::get(QStringLiteral("panel-tree")));

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
        if (!item) {
            // Empty area: actions at the top level.
            tree_->clearSelection();
            tree_->setCurrentItem(nullptr);
            updateNodeActions();
            QMenu menu(this);
            menu.addAction(newConnectionAction_);
            menu.addAction(newFolderAction_);
            menu.exec(tree_->viewport()->mapToGlobal(pos));
            return;
        }
        // Keep a multiple selection when the click lands inside it.
        if (item->isSelected())
            tree_->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
        else
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
            menu.addAction(batchAction_);
            menu.addAction(exportAction_);
            menu.addSeparator();
            menu.addAction(renameAction_);
            menu.addAction(deleteAction_);
        } else {
            menu.addAction(connectAction_);
            if (startTunnelsAction_->isEnabled())
                menu.addAction(startTunnelsAction_);
            menu.addAction(transferAction_);
            menu.addAction(healthAction_);
            menu.addAction(batchAction_);
            // External tools (ping, tracert, ...) against this host.
            QMenu *toolsMenu = menu.addMenu(tr("Strumenti esterni"));
            const QVector<ExternalTool> tools = externaltools::load();
            for (const ExternalTool &tool : tools) {
                QAction *action = toolsMenu->addAction(tool.name);
                action->setToolTip(tool.command);
                connect(action, &QAction::triggered, this,
                        [this, tool, id = node->id] { runExternalTool(tool, id); });
            }
            if (!tools.isEmpty())
                toolsMenu->addSeparator();
            toolsMenu->addAction(externalToolsAction_);
            toolsMenu->setToolTipsVisible(true);
            QAction *probeOne = menu.addAction(tr("Verifica raggiungibilità"));
            probeOne->setEnabled(node->conn.jumpId.isEmpty());
            if (!node->conn.jumpId.isEmpty())
                probeOne->setToolTip(tr("Host raggiungibile solo tramite jump host"));
            const QString probeId = node->id;
            connect(probeOne, &QAction::triggered, this, [this, probeId] { checkReachability(probeId); });
            menu.addAction(favoriteAction_);
            menu.addAction(editAction_);
            menu.addAction(duplicateAction_);
            menu.addAction(renameAction_);
            QMenu *keysMenu = menu.addMenu(tr("Chiavi SSH"));
            keysMenu->addAction(generateKeyAction_);
            keysMenu->addAction(copyKeyAction_);
            menu.addAction(exportAction_);
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
    setupResizableHeader(sessionList_, QStringLiteral("sessions"), 160);
    sessionDock_->setWidget(sessionList_);
    sessionDock_->setMinimumWidth(260);
    sessionDock_->toggleViewAction()->setIcon(
        icons::get(QStringLiteral("panel-list")));
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

    // Tunnels of the open sessions and of the background services, in a tab
    // next to "Sessioni".
    tunnelDock_ = new QDockWidget(tr("Tunnel"), this);
    tunnelDock_->setObjectName(QStringLiteral("tunnelsDock"));
    tunnelDock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea
                                 | Qt::BottomDockWidgetArea);

    tunnelList_ = new QTreeWidget(tunnelDock_);
    tunnelList_->setColumnCount(TunnelColumnCount);
    tunnelList_->setHeaderLabels(
        {tr("Tipo"), tr("Locale"), tr("Remoto"), tr("Connessione"), tr("Stato")});
    tunnelList_->setRootIsDecorated(false);
    tunnelList_->setAlternatingRowColors(true);
    tunnelList_->setUniformRowHeights(true);
    tunnelList_->setContextMenuPolicy(Qt::CustomContextMenu);
    setupResizableHeader(tunnelList_, QStringLiteral("tunnelList"), 36);
    tunnelDock_->setWidget(tunnelList_);
    tunnelDock_->toggleViewAction()->setIcon(
        icons::get(QStringLiteral("tunnel")));
    addDockWidget(Qt::LeftDockWidgetArea, tunnelDock_);
    tabifyDockWidget(sessionDock_, tunnelDock_);
    sessionDock_->raise();

    connect(tunnelList_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item, int) {
        if (!item)
            return;
        if (SessionView *session = tunnelRowSession(item)) {
            if (!locked_)
                activateSession(session);
        } else if (TunnelService *service = tunnelRowService(item)) {
            // Double click on a dropped service: reconnect now.
            if (service->state() == TunnelService::State::Retrying
                || service->state() == TunnelService::State::Failed)
                service->restart();
        }
    });
    connect(tunnelList_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QTreeWidgetItem *item = tunnelList_->itemAt(pos);
        if (!item)
            return;
        SessionView *session = tunnelRowSession(item);
        TunnelService *service = tunnelRowService(item);
        if (!session && !service)
            return;
        const auto type = static_cast<TunnelType>(item->data(0, TunnelTypeRole).toInt());
        const QString address = item->data(0, TunnelAddressRole).toString();

        QMenu menu(this);
        QAction *activate = nullptr;
        QAction *restart = nullptr;
        QAction *stopService = nullptr;
        QAction *copyError = nullptr;
        QAction *copyLog = nullptr;
        if (session) {
            if (locked_)
                return;
            activate = menu.addAction(tr("Vai alla sessione"));
        } else {
            restart = menu.addAction(tr("Riavvia"));
            stopService = menu.addAction(tr("Ferma tunnel di %1").arg(service->connection().displayName()));
            if (!service->lastError().isEmpty())
                copyError = menu.addAction(tr("Copia messaggio di errore"));
            copyLog = menu.addAction(tr("Copia log di ssh"));
        }
        QAction *copy = nullptr;
        QAction *browse = nullptr;
        if (type != TunnelType::Remote) {
            menu.addSeparator();
            copy = menu.addAction(tr("Copia indirizzo locale"));
            if (type == TunnelType::Local)
                browse = menu.addAction(tr("Apri nel browser"));
        }

        QAction *chosen = menu.exec(tunnelList_->viewport()->mapToGlobal(pos));
        if (!chosen)
            return;
        // The menu loop may have let the service or session go away.
        if (session && !sessions_.contains(session))
            return;
        if (service && !tunnelServices_.contains(service))
            return;
        if (chosen == activate)
            activateSession(session);
        else if (chosen == restart)
            service->restart();
        else if (chosen == stopService)
            stopTunnelService(service);
        else if (chosen == copyError)
            QGuiApplication::clipboard()->setText(service->lastError());
        else if (chosen == copyLog)
            QGuiApplication::clipboard()->setText(service->log());
        else if (chosen == copy)
            QGuiApplication::clipboard()->setText(address);
        else if (chosen == browse)
            QDesktopServices::openUrl(QUrl(QStringLiteral("http://") + address));
    });

    // The listening state changes on its own (authentication, ssh exiting)
    // and services count down to their next attempt: refresh while visible.
    tunnelTimer_ = new QTimer(this);
    tunnelTimer_->setInterval(1000);
    connect(tunnelTimer_, &QTimer::timeout, this, &MainWindow::refreshTunnelStatus);
    connect(tunnelDock_, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (visible) {
            refreshTunnelStatus();
            tunnelTimer_->start();
        } else {
            tunnelTimer_->stop();
        }
    });

    createCommandDocks();
}

void MainWindow::createCommandDocks()
{
    // --- Snippets -------------------------------------------------------
    snippetDock_ = new QDockWidget(tr("Snippet"), this);
    snippetDock_->setObjectName(QStringLiteral("snippetsDock"));
    snippetDock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea
                                  | Qt::BottomDockWidgetArea);
    auto *snippetPanel = new QWidget(snippetDock_);
    auto *snippetLayout = new QVBoxLayout(snippetPanel);
    snippetLayout->setContentsMargins(6, 6, 6, 6);
    snippetLayout->setSpacing(6);

    auto *snippetTop = new QHBoxLayout();
    snippetTop->setSpacing(4);
    snippetFilter_ = new QLineEdit(snippetPanel);
    snippetFilter_->setPlaceholderText(tr("Filtra snippet…"));
    snippetFilter_->setClearButtonEnabled(true);
    auto *addSnippet = new QToolButton(snippetPanel);
    addSnippet->setDefaultAction(newSnippetAction_);
    addSnippet->setAutoRaise(true);
    snippetTop->addWidget(snippetFilter_, 1);
    snippetTop->addWidget(addSnippet);
    snippetLayout->addLayout(snippetTop);

    snippetList_ = new QTreeWidget(snippetPanel);
    snippetList_->setColumnCount(2);
    snippetList_->setHeaderLabels({tr("Nome"), tr("Comando")});
    snippetList_->setRootIsDecorated(false);
    snippetList_->setAlternatingRowColors(true);
    snippetList_->setUniformRowHeights(true);
    snippetList_->setContextMenuPolicy(Qt::CustomContextMenu);
    setupResizableHeader(snippetList_, QStringLiteral("snippets"), 140);
    snippetLayout->addWidget(snippetList_, 1);
    snippetDock_->setWidget(snippetPanel);
    snippetDock_->toggleViewAction()->setIcon(
        icons::get(QStringLiteral("snippet")));
    addDockWidget(Qt::LeftDockWidgetArea, snippetDock_);
    tabifyDockWidget(tunnelDock_, snippetDock_);

    connect(snippetFilter_, &QLineEdit::textChanged, this, &MainWindow::rebuildSnippetList);
    connect(snippetList_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item, int) {
        if (Snippet *snippet = item ? store_.findSnippet(item->data(0, Qt::UserRole).toString())
                                    : nullptr)
            sendCommand(snippet->command, snippet->runImmediately, false, snippet->name);
    });
    connect(snippetList_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        if (locked_)
            return;
        QTreeWidgetItem *item = snippetList_->itemAt(pos);
        const QString id = item ? item->data(0, Qt::UserRole).toString() : QString();
        QMenu menu(this);
        QAction *sendActive = nullptr;
        QAction *sendAll = nullptr;
        QAction *edit = nullptr;
        QAction *duplicate = nullptr;
        QAction *remove = nullptr;
        if (!id.isEmpty()) {
            sendActive = menu.addAction(tr("Invia alla sessione attiva"));
            sendActive->setEnabled(activeSession() != nullptr);
            sendAll = menu.addAction(tr("Invia alle sessioni incluse nella trasmissione"));
            sendAll->setEnabled(!sessions_.isEmpty());
            menu.addSeparator();
            edit = menu.addAction(tr("Modifica…"));
            duplicate = menu.addAction(tr("Duplica"));
            remove = menu.addAction(tr("Elimina"));
            menu.addSeparator();
        }
        menu.addAction(newSnippetAction_);
        QAction *chosen = menu.exec(snippetList_->viewport()->mapToGlobal(pos));
        Snippet *snippet = chosen && !id.isEmpty() ? store_.findSnippet(id) : nullptr;
        if (!snippet)
            return;
        if (chosen == sendActive || chosen == sendAll) {
            const Snippet copy = *snippet;
            sendCommand(copy.command, copy.runImmediately, chosen == sendAll, copy.name);
        } else if (chosen == edit) {
            editSnippet(id);
        } else if (chosen == duplicate) {
            Snippet copy = *snippet;
            copy.id = ConnectionStore::newId();
            copy.name += tr(" (copia)");
            store_.snippets.append(copy);
            persist();
            rebuildSnippetList();
        } else if (chosen == remove) {
            if (QMessageBox::question(this, tr("Elimina snippet"),
                                      tr("Eliminare lo snippet \"%1\"?").arg(snippet->name))
                != QMessageBox::Yes)
                return;
            for (int i = 0; i < store_.snippets.size(); ++i) {
                if (store_.snippets.at(i).id == id) {
                    store_.snippets.removeAt(i);
                    break;
                }
            }
            persist();
            rebuildSnippetList();
        }
    });

    // --- Command history of the active session's connection -------------
    historyDock_ = new QDockWidget(tr("Cronologia"), this);
    historyDock_->setObjectName(QStringLiteral("historyDock"));
    historyDock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea
                                  | Qt::BottomDockWidgetArea);
    historyList_ = new QTreeWidget(historyDock_);
    historyList_->setColumnCount(1);
    historyList_->setHeaderHidden(true);
    historyList_->setRootIsDecorated(false);
    historyList_->setAlternatingRowColors(true);
    historyList_->setUniformRowHeights(true);
    historyList_->setContextMenuPolicy(Qt::CustomContextMenu);
    historyList_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    historyDock_->setWidget(historyList_);
    historyDock_->toggleViewAction()->setIcon(
        icons::get(QStringLiteral("history")));
    addDockWidget(Qt::LeftDockWidgetArea, historyDock_);
    tabifyDockWidget(snippetDock_, historyDock_);
    sessionDock_->raise();

    connect(historyList_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item, int) {
        if (item)
            sendCommand(item->text(0), true, false, QString());
    });
    connect(historyList_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        if (locked_ || historyConnectionId_.isEmpty())
            return;
        QTreeWidgetItem *item = historyList_->itemAt(pos);
        const QString command = item ? item->text(0) : QString();
        QMenu menu(this);
        QAction *run = nullptr;
        QAction *insert = nullptr;
        QAction *save = nullptr;
        QAction *copy = nullptr;
        QAction *remove = nullptr;
        if (!command.isEmpty()) {
            const bool canSend = activeSession() != nullptr;
            run = menu.addAction(tr("Esegui nella sessione attiva"));
            run->setEnabled(canSend);
            insert = menu.addAction(tr("Inserisci senza eseguire"));
            insert->setEnabled(canSend);
            save = menu.addAction(tr("Salva come snippet…"));
            copy = menu.addAction(tr("Copia"));
            menu.addSeparator();
            remove = menu.addAction(tr("Elimina dalla cronologia"));
        }
        QAction *clear = menu.addAction(tr("Svuota cronologia"));
        clear->setEnabled(historyList_->topLevelItemCount() > 0);
        QAction *chosen = menu.exec(historyList_->viewport()->mapToGlobal(pos));
        if (!chosen)
            return;
        if (chosen == run || chosen == insert) {
            sendCommand(command, chosen == run, false, QString());
        } else if (chosen == save) {
            Snippet snippet;
            snippet.command = command;
            newSnippet(snippet);
        } else if (chosen == copy) {
            QGuiApplication::clipboard()->setText(command);
        } else if (chosen == remove) {
            store_.history[historyConnectionId_].removeAll(command);
            historySaveTimer_->start();
            rebuildHistoryList();
        } else if (chosen == clear) {
            if (QMessageBox::question(this, tr("Svuota cronologia"),
                                      tr("Eliminare tutti i comandi registrati per %1?")
                                          .arg(historyDock_->windowTitle()))
                != QMessageBox::Yes)
                return;
            store_.history.remove(historyConnectionId_);
            historySaveTimer_->start();
            rebuildHistoryList();
        }
    });

    // History changes often: save it shortly after the last command instead
    // of re-encrypting the vault on every Enter.
    historySaveTimer_ = new QTimer(this);
    historySaveTimer_->setSingleShot(true);
    historySaveTimer_->setInterval(5000);
    connect(historySaveTimer_, &QTimer::timeout, this, [this] { persist(); });
}

void MainWindow::rebuildSnippetList()
{
    if (!snippetList_)
        return;
    snippetList_->clear();
    const QString needle = snippetFilter_->text().trimmed();
    for (const Snippet &snippet : store_.snippets) {
        if (!needle.isEmpty() && !snippet.name.contains(needle, Qt::CaseInsensitive)
            && !snippet.command.contains(needle, Qt::CaseInsensitive)
            && !snippet.description.contains(needle, Qt::CaseInsensitive))
            continue;
        auto *item = new QTreeWidgetItem(snippetList_);
        item->setText(0, snippet.name);
        item->setText(1, QString(snippet.command).replace(QLatin1Char('\n'), QStringLiteral(" ⏎ ")));
        QString tip = snippet.command.toHtmlEscaped();
        if (!snippet.description.isEmpty())
            tip = QStringLiteral("<b>%1</b><br>").arg(snippet.description.toHtmlEscaped()) + tip;
        item->setToolTip(0, QStringLiteral("<pre style=\"white-space:pre-wrap\">%1</pre>").arg(tip));
        item->setToolTip(1, item->toolTip(0));
        item->setData(0, Qt::UserRole, snippet.id);
    }
    snippetList_->sortItems(0, Qt::AscendingOrder);
}

void MainWindow::rebuildHistoryList()
{
    if (!historyList_)
        return;

    // The active session's connection, else the host selected in the tree.
    QString id;
    QString name;
    if (!locked_) {
        if (SessionView *session = activeSession(); session && !session->connection().local) {
            id = session->connection().id;
            name = session->connection().displayName();
        } else if (const Node *node = store_.findNode(selectedId());
                   node && node->kind == Node::Kind::Host) {
            id = node->id;
            name = node->conn.displayName();
        }
    }
    historyConnectionId_ = id;
    historyDock_->setWindowTitle(name.isEmpty() ? tr("Cronologia")
                                                : tr("Cronologia — %1").arg(name));
    historyList_->clear();
    for (const QString &command : store_.history.value(id)) {
        auto *item = new QTreeWidgetItem(historyList_);
        item->setText(0, command);
        item->setToolTip(0, command);
    }
}

void MainWindow::recordCommand(SessionView *session, const QString &command)
{
    if (locked_ || !session)
        return;
    const QString id = session->connection().id;
    if (!store_.findConnection(id))
        return; // quick connections are not saved, neither is their history
    store_.addHistory(id, command);
    historySaveTimer_->start();
    if (id == historyConnectionId_)
        rebuildHistoryList();
}

void MainWindow::sendCommand(const QString &text, bool execute, bool toBroadcast,
                             const QString &title)
{
    if (locked_ || text.isEmpty())
        return;

    QList<SessionView *> targets;
    if (toBroadcast) {
        for (SessionView *session : sessions_) {
            if (!session->isFinished() && session->isIncludedInBroadcast())
                targets << session;
        }
    } else if (SessionView *session = activeSession(); session && !session->isFinished()) {
        targets << session;
    }
    if (targets.isEmpty()) {
        statusBar()->showMessage(toBroadcast ? tr("Nessuna sessione inclusa nella trasmissione")
                                             : tr("Nessuna sessione attiva"),
                                 5000);
        return;
    }

    // Custom ${variables} are asked once and shared by every target.
    QString command = text;
    const QStringList variables = snippets::customVariables(command);
    if (!variables.isEmpty()) {
        if (!askSnippetVariables(this, title.isEmpty() ? tr("Variabili") : title, variables,
                                 &snippetValues_))
            return;
        command = snippets::applyVariables(command, snippetValues_);
    }

    for (SessionView *session : targets) {
        const QString expanded = snippets::expandBuiltins(command, session->connection());
        session->sendText(execute ? expanded + QLatin1Char('\n') : expanded);
        if (execute && !expanded.contains(QLatin1Char('\n')))
            recordCommand(session, expanded);
    }
    if (SessionView *active = activeSession())
        active->terminal()->setFocus();
}

void MainWindow::newSnippet(const Snippet &prefill)
{
    if (locked_)
        return;
    SnippetDialog dialog(prefill, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    Snippet snippet = dialog.snippet();
    snippet.id = ConnectionStore::newId();
    store_.snippets.append(snippet);
    persist();
    rebuildSnippetList();
    snippetDock_->show();
    snippetDock_->raise();
}

void MainWindow::editSnippet(const QString &id)
{
    Snippet *existing = locked_ ? nullptr : store_.findSnippet(id);
    if (!existing)
        return;
    SnippetDialog dialog(*existing, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (Snippet *current = store_.findSnippet(id)) {
        *current = dialog.snippet();
        persist();
        rebuildSnippetList();
    }
}

SessionView *MainWindow::tunnelRowSession(QTreeWidgetItem *item) const
{
    if (!item || item->data(0, TunnelKindRole).toInt() != TunnelFromSession)
        return nullptr;
    auto *session =
        reinterpret_cast<SessionView *>(item->data(0, TunnelOwnerRole).toULongLong());
    return sessions_.contains(session) ? session : nullptr;
}

TunnelService *MainWindow::tunnelRowService(QTreeWidgetItem *item) const
{
    if (!item || item->data(0, TunnelKindRole).toInt() != TunnelFromService)
        return nullptr;
    auto *service =
        reinterpret_cast<TunnelService *>(item->data(0, TunnelOwnerRole).toULongLong());
    return tunnelServices_.contains(service) ? service : nullptr;
}

void MainWindow::rebuildTunnelList()
{
    if (!tunnelList_)
        return;

    const auto hostPort = [](QString host, quint16 port, const QString &fallback) {
        host = host.trimmed();
        if (host.isEmpty())
            host = fallback;
        if (host.contains(QLatin1Char(':')))
            host = QLatin1Char('[') + host + QLatin1Char(']');
        return host.isEmpty() ? QString::number(port) : host + QLatin1Char(':') + QString::number(port);
    };

    const QIcon sessionIcon = style()->standardIcon(QStyle::SP_ComputerIcon);
    const QIcon serviceIcon = QIcon::fromTheme(QStringLiteral("network-server"),
                                               style()->standardIcon(QStyle::SP_DriveNetIcon));

    const auto addRows = [&](const Connection &connection, int kind, quintptr owner,
                             const QString &originTip) {
        for (const TunnelConfig &tunnel : connection.tunnels) {
            if (!tunnel.enabled)
                continue;
            QString type;
            QString typeTip;
            QString local;
            QString remote;
            switch (tunnel.type) {
            case TunnelType::Local:
                type = QStringLiteral("-L");
                typeTip = tr("Locale: porta su questo computer inoltrata a una destinazione "
                             "raggiunta dal server");
                local = hostPort(tunnel.localAddress, tunnel.localPort, QStringLiteral("localhost"));
                remote = hostPort(tunnel.remoteHost, tunnel.remotePort, QString());
                break;
            case TunnelType::Remote:
                type = QStringLiteral("-R");
                typeTip = tr("Remoto: porta sul server inoltrata a una destinazione raggiunta da "
                             "questo computer");
                local = hostPort(tunnel.localAddress, tunnel.localPort, QStringLiteral("localhost"));
                remote = tr("server %1").arg(hostPort(tunnel.remoteHost, tunnel.remotePort, QString()));
                break;
            case TunnelType::Dynamic:
                type = QStringLiteral("-D");
                typeTip = tr("Dinamico: proxy SOCKS su questo computer");
                local = hostPort(tunnel.localAddress, tunnel.localPort, QStringLiteral("localhost"));
                remote = tr("SOCKS");
                break;
            }

            auto *item = new QTreeWidgetItem(tunnelList_);
            item->setText(TunnelTypeColumn, type);
            item->setToolTip(TunnelTypeColumn, typeTip);
            item->setText(TunnelLocalColumn, local);
            item->setText(TunnelRemoteColumn, remote);
            item->setToolTip(TunnelLocalColumn,
                             tr("%1 → %2 tramite %3").arg(local, remote, connection.target()));
            item->setText(TunnelConnectionColumn, connection.displayName());
            item->setIcon(TunnelConnectionColumn,
                          kind == TunnelFromService ? serviceIcon : sessionIcon);
            item->setToolTip(TunnelConnectionColumn, originTip);
            item->setData(0, TunnelOwnerRole, QVariant::fromValue<qulonglong>(owner));
            item->setData(0, TunnelKindRole, kind);
            item->setData(0, TunnelTypeRole, int(tunnel.type));
            item->setData(0, TunnelPortRole, int(tunnel.localPort));
            item->setData(0, TunnelAddressRole, local);
        }
    };

    tunnelList_->clear();
    for (TunnelService *service : tunnelServices_)
        addRows(service->connection(), TunnelFromService, reinterpret_cast<quintptr>(service),
                tr("Tunnel in background, senza terminale"));
    for (SessionView *session : sessions_)
        addRows(session->connection(), TunnelFromSession, reinterpret_cast<quintptr>(session),
                tr("Tunnel della sessione %1").arg(session->title()));

    refreshTunnelStatus();
    const int count = tunnelList_->topLevelItemCount();
    tunnelDock_->setWindowTitle(count > 0 ? tr("Tunnel (%1)").arg(count) : tr("Tunnel"));
}

void MainWindow::refreshTunnelStatus()
{
    if (!tunnelList_)
        return;

    // One socket-table read per ssh process.
    QHash<qint64, QSet<quint16>> portsByPid;
    bool supported = true;
    const QColor good = apptheme::success();
    const QColor waiting = apptheme::warning();
    const QColor failed = apptheme::danger();
    const QColor muted = apptheme::muted();

    // Local and SOCKS forwards are checked in the socket table; a remote
    // forward listens on the server and only ssh can confirm it.
    const auto listening = [&](qint64 pid, TunnelType type, quint16 port, QString *state,
                               QString *tip, QColor *color) {
        if (type == TunnelType::Remote)
            return false;
        if (!portsByPid.contains(pid))
            portsByPid.insert(pid, tunnelprobe::listeningPorts(pid, &supported));
        if (!supported)
            return false;
        if (portsByPid.value(pid).contains(port)) {
            *state = tr("in ascolto");
            *tip = tr("La porta locale è aperta da ssh");
            *color = good;
        } else {
            *state = tr("in attesa");
            *tip = tr("ssh non ha (ancora) aperto la porta: autenticazione in corso, "
                      "oppure porta già occupata da un altro programma");
            *color = waiting;
        }
        return true;
    };

    for (int i = 0; i < tunnelList_->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = tunnelList_->topLevelItem(i);
        const auto type = static_cast<TunnelType>(item->data(0, TunnelTypeRole).toInt());
        const auto port = quint16(item->data(0, TunnelPortRole).toInt());

        QString state;
        QString tip;
        QColor color = muted;
        if (SessionView *session = tunnelRowSession(item)) {
            const qint64 pid = session->isFinished() ? 0 : session->terminal()->getShellPID();
            if (pid <= 0) {
                state = tr("chiuso");
                tip = tr("La sessione è terminata");
            } else if (!listening(pid, type, port, &state, &tip, &color)) {
                state = tr("attivo");
                tip = type == TunnelType::Remote
                          ? tr("La porta è sul server: in una sessione con terminale lo stato "
                               "non è verificabile. Avvia i tunnel in background per averne "
                               "la conferma.")
                          : QString();
                color = palette().color(QPalette::Text);
            }
        } else if (TunnelService *service = tunnelRowService(item)) {
            tip = service->lastError();
            switch (service->state()) {
            case TunnelService::State::Starting:
                state = tr("in avvio");
                tip = tr("Connessione e autenticazione in corso");
                color = waiting;
                break;
            case TunnelService::State::Running:
                if (!listening(service->processId(), type, port, &state, &tip, &color)) {
                    state = type == TunnelType::Remote ? tr("in ascolto sul server") : tr("attivo");
                    tip = tr("Inoltro confermato da ssh");
                    color = good;
                }
                break;
            case TunnelService::State::Retrying:
                state = tr("riconnessione tra %1 s").arg(service->secondsToRetry());
                color = waiting;
                break;
            case TunnelService::State::Failed:
                state = tr("errore");
                color = failed;
                break;
            case TunnelService::State::Stopped:
                state = tr("fermato");
                break;
            }
        } else {
            continue;
        }
        item->setText(TunnelStateColumn, state);
        item->setToolTip(TunnelStateColumn, tip);
        item->setForeground(TunnelStateColumn, color);
    }
}

TunnelService *MainWindow::tunnelServiceFor(const QString &connectionId) const
{
    for (TunnelService *service : tunnelServices_) {
        if (service->connection().id == connectionId)
            return service;
    }
    return nullptr;
}

void MainWindow::startTunnelService(const QString &id)
{
    if (locked_ || id.isEmpty())
        return;

    // One background service per connection: start again only a dead one.
    if (TunnelService *existing = tunnelServiceFor(id)) {
        if (existing->state() == TunnelService::State::Starting
            || existing->state() == TunnelService::State::Running) {
            statusBar()->showMessage(tr("I tunnel di %1 sono già attivi")
                                         .arg(existing->connection().displayName()),
                                     5000);
        } else {
            existing->restart();
        }
        tunnelDock_->show();
        tunnelDock_->raise();
        return;
    }

    Connection effective;
    QVector<Connection> chain;
    if (!resolveConnection(id, &effective, &chain))
        return;

    const SshPlan plan = buildTunnelPlan(effective, chain, paths::runtimeDir());
    effective.clearSensitive();
    for (Connection &hop : chain)
        hop.clearSensitive();

    if (!plan.valid) {
        for (const QString &path : plan.temporaryFiles)
            QFile::remove(path);
        QMessageBox::warning(this, tr("Impossibile avviare i tunnel"), plan.error);
        return;
    }

    auto *service = new TunnelService(effective, plan, this);
    tunnelServices_.append(service);
    connect(service, &TunnelService::stateChanged, this, [this, service] {
        refreshTunnelStatus();
        handleTunnelServiceState(service);
    });
    service->start();

    rebuildTunnelList();
    tunnelDock_->show();
    tunnelDock_->raise();
    statusBar()->showMessage(
        tr("Tunnel di %1 avviati in background").arg(effective.displayName()), 5000);
}

void MainWindow::stopTunnelService(TunnelService *service)
{
    if (!tunnelServices_.removeAll(service))
        return;
    const QString name = service->connection().displayName();
    serviceStates_.remove(service);
    droppedServices_.remove(service);
    service->disconnect(this);
    service->stop();
    service->deleteLater(); // also deletes its key/askpass files
    rebuildTunnelList();
    updateNodeActions();
    updateTray();
    statusBar()->showMessage(tr("Tunnel di %1 fermati").arg(name), 5000);
}

void MainWindow::handleTunnelServiceState(TunnelService *service)
{
    using State = TunnelService::State;
    const State state = service->state();
    const State previous = static_cast<State>(serviceStates_.value(service, int(State::Stopped)));
    serviceStates_.insert(service, int(state));
    bool hostKeyFailure = !tray_ && state == State::Failed && state != previous;

    // Notify transitions only: a drop of a working tunnel, a give-up, and
    // the recovery of a tunnel that had dropped.
    if (tray_ && state != previous) {
        const QString name = service->connection().displayName();
        if (state == State::Retrying && previous == State::Running) {
            droppedServices_.insert(service);
            tray_->showMessage(tr("Tunnel di %1 interrotto").arg(name),
                               tr("%1\nNuovo tentativo tra %2 s.")
                                   .arg(service->lastError())
                                   .arg(service->secondsToRetry()),
                               QSystemTrayIcon::Warning, 8000);
        } else if (state == State::Failed) {
            droppedServices_.remove(service);
            tray_->showMessage(tr("Tunnel di %1 in errore").arg(name), service->lastError(),
                               QSystemTrayIcon::Critical, 10000);
            hostKeyFailure = true;
        } else if (state == State::Running && droppedServices_.remove(service)) {
            tray_->showMessage(tr("Tunnel di %1 di nuovo attivo").arg(name), QString(),
                               QSystemTrayIcon::Information, 5000);
        }
    }
    updateTray();

    // A changed host key deserves an explanation, not just ssh's last line.
    const QString log = service->log();
    if (hostKeyFailure && log.contains(QLatin1String("REMOTE HOST IDENTIFICATION HAS CHANGED"))) {
        QString hostName = knownhosts::hostName(service->connection().host,
                                                service->connection().port);
        const QRegularExpressionMatch match =
            QRegularExpression(QStringLiteral("Host key for\\s+(\\S+)\\s+has\\s+changed")).match(log);
        if (match.hasMatch())
            hostName = match.captured(1);
        const QRegularExpressionMatch key =
            QRegularExpression(QStringLiteral("sent by the remote host is\\s+(SHA256:[A-Za-z0-9+/=]+)"))
                .match(log);
        // Hidden in the tray: the notification already told the user.
        if (isVisible())
            handleHostKeyChanged(hostName, key.hasMatch() ? key.captured(1) : QString(),
                                 service->connection().id, nullptr, service);
    }
}

void MainWindow::handleHostKeyChanged(const QString &hostName, const QString &newFingerprint,
                                      const QString &connectionId, SessionView *session,
                                      TunnelService *service)
{
    const QPointer<SessionView> guardedSession(session);
    const QPointer<TunnelService> guardedService(service);

    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Chiave host cambiata"));
    box.setText(tr("<b>La chiave di %1 non corrisponde a quella salvata.</b>")
                    .arg(hostName.toHtmlEscaped()));
    box.setInformativeText(
        tr("Succede quando il server viene reinstallato o la sua chiave rigenerata, ma può "
           "anche indicare un attacco man-in-the-middle. Prima di accettare la nuova chiave, "
           "verifica con l'amministratore del server che il cambio sia previsto.\n\n"
           "Rimuovendo la chiave salvata, alla riconnessione verrà mostrato il nuovo "
           "fingerprint da confermare."));
    if (!newFingerprint.isEmpty())
        box.setDetailedText(tr("Fingerprint della chiave presentata ora dal server:\n%1")
                                .arg(newFingerprint));
    QPushButton *reconnect = nullptr;
    const bool canReconnect =
        service || (!locked_ && !connectionId.isEmpty() && store_.findConnection(connectionId));
    if (canReconnect)
        reconnect = box.addButton(tr("Rimuovi la chiave salvata e riconnetti"),
                                  QMessageBox::AcceptRole);
    QPushButton *manage = box.addButton(tr("Gestisci chiavi host…"), QMessageBox::ActionRole);
    box.addButton(QMessageBox::Close);
    box.setDefaultButton(QMessageBox::Close);
    box.exec();

    if (box.clickedButton() == manage) {
        showKnownHosts(hostName);
        return;
    }
    if (!reconnect || box.clickedButton() != reconnect)
        return;

    QString error;
    if (knownhosts::removeHost(paths::knownHostsFile(), hostName, &error) < 0) {
        QMessageBox::warning(this, tr("Chiave host cambiata"),
                             tr("Impossibile aggiornare il file delle chiavi: %1").arg(error));
        return;
    }
    if (guardedService) {
        if (tunnelServices_.contains(guardedService.data()))
            guardedService->restart();
        return;
    }
    // Replace the dead session window with a fresh connection.
    if (guardedSession) {
        const auto windows = mdi_->subWindowList();
        for (QMdiSubWindow *sub : windows) {
            if (sub->widget() == guardedSession.data()) {
                sub->close();
                break;
            }
        }
    }
    if (!locked_ && store_.findConnection(connectionId))
        openConnection(connectionId);
}

void MainWindow::showKnownHosts(const QString &filter)
{
    QVector<QPair<QString, QString>> connections;
    if (!locked_) {
        QVector<const Connection *> all;
        store_.collectConnections(all);
        for (const Connection *connection : all)
            connections.append({connection->displayName(),
                                knownhosts::hostName(connection->host, connection->port)});
    }
    KnownHostsDialog dialog(connections, filter, this);
    dialog.exec();
}

void MainWindow::createTray()
{
    // Application icon: a terminal prompt on a dark rounded square.
    const auto drawBase = [](int size) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.scale(size / 32.0, size / 32.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0x2d, 0x33, 0x3b));
        painter.drawRoundedRect(QRectF(1, 3, 30, 26), 5, 5);
        QPen pen(QColor(0xe6, 0xed, 0xf3), 2.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(pen);
        painter.drawPolyline(QPolygonF({QPointF(7.5, 10), QPointF(13, 15.5), QPointF(7.5, 21)}));
        painter.drawLine(QPointF(15.5, 21.5), QPointF(23.5, 21.5));
        return pixmap;
    };
    for (int size : {16, 24, 32, 48, 64, 128})
        appIcon_.addPixmap(drawBase(size));
    setWindowIcon(appIcon_);

    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;

    tray_ = new QSystemTrayIcon(this);
    trayMenu_ = new QMenu(this);
    tray_->setContextMenu(trayMenu_);
    connect(trayMenu_, &QMenu::aboutToShow, this, &MainWindow::rebuildTrayMenu);
    connect(tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger
                    || reason == QSystemTrayIcon::DoubleClick)
                    showFromTray();
            });
    connect(tray_, &QSystemTrayIcon::messageClicked, this, [this] {
        showFromTray();
        tunnelDock_->show();
        tunnelDock_->raise();
    });
    // Windows needs a menu at creation time to show it on the first click.
    rebuildTrayMenu();
    updateTray();
    tray_->show();
}

void MainWindow::updateTray()
{
    if (!tray_)
        return;

    using State = TunnelService::State;
    int running = 0;
    int pending = 0;
    int failed = 0;
    for (TunnelService *service : tunnelServices_) {
        switch (service->state()) {
        case State::Running:
            ++running;
            break;
        case State::Starting:
        case State::Retrying:
            ++pending;
            break;
        case State::Failed:
            ++failed;
            break;
        case State::Stopped:
            break;
        }
    }

    QColor dot;
    if (failed > 0)
        dot = apptheme::danger();
    else if (pending > 0)
        dot = apptheme::warning();
    else if (running > 0)
        dot = apptheme::success();

    if (!dot.isValid()) {
        tray_->setIcon(appIcon_);
    } else {
        QIcon icon;
        for (int size : {16, 24, 32, 48, 64}) {
            QPixmap pixmap = appIcon_.pixmap(size, size);
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing, true);
            const qreal d = size * 0.5;
            painter.setPen(QPen(Qt::white, qMax(1.0, size / 16.0)));
            painter.setBrush(dot);
            painter.drawEllipse(QRectF(size - d - 0.5, size - d - 0.5, d, d));
            painter.end();
            icon.addPixmap(pixmap);
        }
        tray_->setIcon(icon);
    }

    QStringList parts;
    if (running > 0)
        parts << tr("attivi: %1").arg(running);
    if (pending > 0)
        parts << tr("in riconnessione: %1").arg(pending);
    if (failed > 0)
        parts << tr("in errore: %1").arg(failed);
    tray_->setToolTip(parts.isEmpty()
                          ? tr("SSH Manager")
                          : tr("SSH Manager — tunnel: %1").arg(parts.join(QStringLiteral(", "))));
}

void MainWindow::rebuildTrayMenu()
{
    if (!trayMenu_)
        return;
    trayMenu_->clear();

    QAction *show = trayMenu_->addAction(tr("Mostra SSH Manager"), this, &MainWindow::showFromTray);
    QFont bold = show->font();
    bold.setBold(true);
    show->setFont(bold);
    trayMenu_->addSeparator();

    if (tunnelServices_.isEmpty()) {
        trayMenu_->addAction(tr("Nessun tunnel in background"))->setEnabled(false);
    } else {
        trayMenu_->addSection(tr("Tunnel in background"));
        for (TunnelService *service : tunnelServices_) {
            QString state;
            switch (service->state()) {
            case TunnelService::State::Starting:
                state = tr("in avvio");
                break;
            case TunnelService::State::Running:
                state = tr("attivo");
                break;
            case TunnelService::State::Retrying:
                state = tr("riconnessione tra %1 s").arg(service->secondsToRetry());
                break;
            case TunnelService::State::Failed:
                state = tr("errore");
                break;
            case TunnelService::State::Stopped:
                state = tr("fermato");
                break;
            }
            QMenu *sub = trayMenu_->addMenu(
                tr("%1 — %2").arg(service->connection().displayName(), state));
            sub->addAction(tr("Riavvia"), this, [this, service] {
                if (tunnelServices_.contains(service))
                    service->restart();
            });
            sub->addAction(tr("Ferma"), this, [this, service] {
                if (tunnelServices_.contains(service))
                    stopTunnelService(service);
            });
        }
    }

    trayMenu_->addSeparator();
    QAction *keep = trayMenu_->addAction(tr("Resta nella tray alla chiusura"));
    keep->setCheckable(true);
    keep->setChecked(keepInTray_);
    keep->setToolTip(tr("Chiudendo la finestra con tunnel in background attivi, "
                        "l'app resta nella tray invece di fermarli"));
    connect(keep, &QAction::toggled, this, [this](bool on) {
        keepInTray_ = on;
        QSettings().setValue(QStringLiteral("view/keepInTray"), on);
    });
    trayMenu_->addAction(tr("Esci"), this, &MainWindow::quitApplication);
}

void MainWindow::showFromTray()
{
    if (isMinimized())
        showNormal();
    else
        show();
    raise();
    activateWindow();
    if (locked_)
        QTimer::singleShot(0, this, &MainWindow::requestUnlock);
}

void MainWindow::quitApplication()
{
    quitting_ = true;
    if (!close()) {
        quitting_ = false;
        return;
    }
    // A window already hidden in the tray does not trigger "last window
    // closed": quit explicitly.
    QCoreApplication::quit();
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
        if (session->isFinished())
            item->setToolTip(1, session->endReason());

        item->setIcon(0, style()->standardIcon(session->isFinished() ? QStyle::SP_MessageBoxWarning
                                                                     : QStyle::SP_ComputerIcon));
        // New output or a bell in a session in the background.
        if (!session->isFinished() && session->attention() != SessionView::Attention::None) {
            const bool bell = session->attention() == SessionView::Attention::Bell;
            QPixmap pixmap = item->icon(0).pixmap(32, 32);
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(QPen(palette().color(QPalette::Base), 2));
            painter.setBrush(bell ? apptheme::warning() : apptheme::info());
            painter.drawEllipse(QRectF(17, 1, 14, 14));
            painter.end();
            item->setIcon(0, QIcon(pixmap));
            item->setToolTip(0, (bell ? tr("Campanello") : tr("Nuovo output")) + QStringLiteral(" — ")
                                    + session->connection().target());
        }
        item->setToolTip(0, session->connection().target());
        item->setData(0, Qt::UserRole,
                      QVariant::fromValue<qulonglong>(reinterpret_cast<quintptr>(session)));

        if (session == active)
            sessionList_->setCurrentItem(item);
    }
    rebuildTunnelList();
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
    auto *item = parentItem ? new NodeItem(parentItem) : new NodeItem(tree_);
    item->setText(0, node.name.isEmpty() ? tr("(senza nome)") : node.name);
    item->setData(0, Qt::UserRole, node.id);
    item->setData(0, Qt::UserRole + 1, static_cast<int>(node.kind));
    // Only folders accept dropped items.
    Qt::ItemFlags flags = item->flags() | Qt::ItemIsDragEnabled;
    if (node.kind == Node::Kind::Folder)
        flags |= Qt::ItemIsDropEnabled;
    else
        flags &= ~Qt::ItemIsDropEnabled;
    item->setFlags(flags);

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
        // Favourites keep the usual icon: the ★ column shows them.
        item->setIcon(0, style()->standardIcon(QStyle::SP_ComputerIcon));
        item->setText(1, node.conn.target());
        if (node.conn.favorite) {
            item->setText(TreeFavoriteColumn, QStringLiteral("★"));
            item->setForeground(TreeFavoriteColumn, apptheme::warning());
        }
        item->setTextAlignment(TreeFavoriteColumn, Qt::AlignCenter);
        item->setText(TreeTagsColumn, node.conn.tags.join(QStringLiteral(", ")));
        item->setData(0, Qt::UserRole + 3, node.conn.favorite);
        item->setData(0, Qt::UserRole + 4, node.conn.tags);
        // The name first: a long one is often cut off in the tree.
        QString tip = node.name.isEmpty() ? node.conn.displayName() : node.name;
        tip += QLatin1Char('\n') + node.conn.target();
        if (!node.conn.keyName.isEmpty())
            tip += QLatin1Char('\n') + tr("Chiave archiviata: %1").arg(node.conn.keyName);
        if (!node.conn.tags.isEmpty())
            tip += QLatin1Char('\n') + tr("Tag: %1").arg(node.conn.tags.join(QStringLiteral(", ")));
        if (!node.conn.notes.isEmpty())
            tip += QStringLiteral("\n") + node.conn.notes;
        item->setData(0, Qt::UserRole + 5, tip);              // tooltip without reachability
        item->setData(0, Qt::UserRole + 6, !node.conn.jumpId.isEmpty());
        for (int column = 0; column < TreeColumnCount; ++column)
            item->setToolTip(column, tip);
        applyReachability(item);
    }

    itemById_.insert(node.id, item);
    return item;
}

void MainWindow::applyReachability(QTreeWidgetItem *item)
{
    if (!item || item->data(0, Qt::UserRole + 1).toInt() != int(Node::Kind::Host))
        return;
    const QString id = item->data(0, Qt::UserRole).toString();
    const bool viaJump = item->data(0, Qt::UserRole + 6).toBool();
    QString tip = item->data(0, Qt::UserRole + 5).toString();

    const QIcon base = style()->standardIcon(QStyle::SP_ComputerIcon);
    const auto it = reachability_.constFind(id);
    if (it == reachability_.cend() || it->state == HostProbe::Result::State::Unknown) {
        item->setIcon(0, base);
        if (reachabilityEnabled_ && viaJump)
            tip += tr("\nRaggiungibilità: non verificata (tramite jump host)");
    } else {
        const bool up = it->state == HostProbe::Result::State::Reachable;
        // A small dot in the corner of the usual icon.
        QPixmap pixmap = base.pixmap(32, 32);
        {
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(QPen(palette().color(QPalette::Base), 2));
            painter.setBrush(up ? apptheme::success() : apptheme::danger());
            painter.drawEllipse(QRectF(17, 17, 14, 14));
        }
        item->setIcon(0, QIcon(pixmap));
        const QString when = QLocale().toString(it->checkedAt.time(), QLocale::ShortFormat);
        if (up) {
            tip += tr("\nRaggiungibile: %1 ms%2 (verificato alle %3)")
                       .arg(it->latencyMs)
                       .arg(it->banner.isEmpty() ? QString() : QStringLiteral(" · ") + it->banner)
                       .arg(when);
        } else {
            tip += tr("\nNon raggiungibile: %1 (verificato alle %2)").arg(it->error, when);
        }
    }
    item->setToolTip(0, tip);
    item->setToolTip(1, tip);
}

SshPlan MainWindow::commandPlan(const QString &id)
{
    SshPlan plan;
    Connection effective;
    QVector<Connection> chain;
    if (locked_ || !store_.findConnection(id)) {
        plan.error = tr("Archivio bloccato: credenziali non disponibili.");
        return plan;
    }
    if (!resolveConnection(id, &effective, &chain)) {
        plan.error = tr("Password non inserita.");
        return plan;
    }
    plan = buildCommandPlan(effective, chain, paths::runtimeDir());
    effective.clearSensitive();
    for (Connection &hop : chain)
        hop.clearSensitive();
    return plan;
}

QStringList MainWindow::selectedHostIds() const
{
    QStringList ids;
    std::function<void(const Node &)> collect = [&](const Node &node) {
        if (node.kind == Node::Kind::Host) {
            if (!ids.contains(node.id))
                ids << node.id;
            return;
        }
        for (const Node &child : node.children)
            collect(child);
    };
    for (QTreeWidgetItem *item : tree_->selectedItems()) {
        if (const Node *node = store_.findNode(item->data(0, Qt::UserRole).toString()))
            collect(*node); // a folder stands for every host inside it
    }
    return ids;
}

void MainWindow::runBatch()
{
    if (locked_)
        return;
    QVector<BatchDialog::Host> hosts;
    QVector<const Connection *> connections;
    store_.collectConnections(connections);
    for (const Connection *connection : connections) {
        BatchDialog::Host host;
        host.id = connection->id;
        host.name = connection->displayName();
        host.connection = *connection;
        host.connection.clearSensitive();
        hosts.append(host);
    }
    auto *dialog = new BatchDialog(hosts, selectedHostIds(), store_.snippets,
                                   [this](const QString &id) { return commandPlan(id); }, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void MainWindow::showHostHealth(const QString &id)
{
    const Connection *connection = locked_ ? nullptr : store_.findConnection(id);
    if (!connection)
        return;
    const QString name = connection->displayName();
    const QString target = connection->target();
    const SshPlan plan = commandPlan(id);
    if (!plan.valid) {
        for (const QString &path : plan.temporaryFiles)
            QFile::remove(path);
        if (!plan.error.isEmpty())
            QMessageBox::warning(this, tr("Stato dell'host"), plan.error);
        return;
    }
    auto *dialog = new HostHealthDialog(name, target, plan, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void MainWindow::notify(const QString &title, const QString &body, QSystemTrayIcon::MessageIcon icon)
{
    if (tray_ && tray_->isVisible())
        tray_->showMessage(title, body, icon, 8000);
    else
        QApplication::alert(this);
}

void MainWindow::updateSessionAttention(SessionView *session)
{
    // The window or tab title carries a marker too: visible in tabbed view
    // and on the MDI title bars.
    for (QMdiSubWindow *sub : mdi_->subWindowList()) {
        if (sub->widget() != session)
            continue;
        QString prefix;
        if (session->attention() == SessionView::Attention::Bell)
            prefix = QStringLiteral("🔔 ");
        else if (session->attention() == SessionView::Attention::Activity)
            prefix = QStringLiteral("● ");
        sub->setWindowTitle(prefix + session->title());
    }
    rebuildSessionList();
}

void MainWindow::handleSessionEnded(SessionView *session, const QString &reason, bool unexpected)
{
    const QString name = session->connection().displayName();
    statusBar()->showMessage(tr("Sessione %1 terminata: %2").arg(name, reason), 10000);
    // Only a drop the user may not have seen is worth a desktop notification.
    if (unexpected && (!isActiveWindow() || activeSession() != session))
        notify(tr("Sessione %1 terminata").arg(name), reason, QSystemTrayIcon::Warning);
}

void MainWindow::reconnectSession(SessionView *old)
{
    if (!sessions_.contains(old))
        return;
    QMdiSubWindow *oldSub = nullptr;
    for (QMdiSubWindow *sub : mdi_->subWindowList()) {
        if (sub->widget() == old)
            oldSub = sub;
    }
    const Connection connection = old->connection();
    const QList<SessionView *> before = sessions_;

    if (connection.local) {
        startSession(connection, old->plan()); // local shell or external tool: same command
    } else if (!locked_ && store_.findConnection(connection.id)) {
        openConnection(connection.id); // fresh credentials and key files
    } else if (old->plan().temporaryFiles.isEmpty()) {
        startSession(connection, old->plan()); // quick connection: nothing stored
    } else {
        statusBar()->showMessage(tr("Sblocca l'archivio per riconnettere %1")
                                     .arg(connection.displayName()),
                                 6000);
        return;
    }

    SessionView *fresh = nullptr;
    for (SessionView *session : sessions_) {
        if (!before.contains(session))
            fresh = session;
    }
    if (!fresh)
        return; // cancelled, e.g. at the password prompt
    // Same place and size as the window it replaces.
    for (QMdiSubWindow *sub : mdi_->subWindowList()) {
        if (sub->widget() == fresh && oldSub && !tabbedView_) {
            if (oldSub->isMaximized())
                sub->showMaximized();
            else
                sub->setGeometry(oldSub->geometry());
        }
    }
    if (oldSub)
        oldSub->close();
}

void MainWindow::checkReachability(const QString &onlyId)
{
    if (locked_)
        return;
    QVector<const Connection *> connections;
    store_.collectConnections(connections);
    for (const Connection *connection : connections) {
        if (!onlyId.isEmpty() && connection->id != onlyId)
            continue;
        // Hosts behind a jump host cannot be reached directly.
        if (!connection->jumpId.isEmpty() || connection->host.isEmpty())
            continue;
        probe_->check(connection->id, connection->host, connection->port);
    }
}

void MainWindow::setReachabilityEnabled(bool enabled)
{
    if (enabled && !QSettings().value(QStringLiteral("view/reachabilityExplained")).toBool()) {
        QSettings().setValue(QStringLiteral("view/reachabilityExplained"), true);
        QMessageBox::information(
            this, tr("Raggiungibilità degli host"),
            tr("Ogni %1 minuti SSH Manager apre una connessione TCP verso ogni host salvato, legge "
               "la versione del server SSH e la chiude subito, senza autenticarsi.\n\n"
               "Il server registra comunque queste connessioni nei suoi log. Su host protetti "
               "da fail2ban in modalità aggressiva potrebbero contare come tentativi sospetti: "
               "in quel caso lascia l'opzione disattivata e usa \"Verifica raggiungibilità\" dal "
               "menu dell'host quando serve.")
                .arg(kReachabilityMinutes));
    }
    reachabilityEnabled_ = enabled;
    QSettings().setValue(QStringLiteral("view/reachability"), enabled);
    {
        const QSignalBlocker blocker(reachabilityAction_);
        reachabilityAction_->setChecked(enabled);
    }
    if (enabled) {
        reachabilityTimer_->start();
        checkReachability();
    } else {
        reachabilityTimer_->stop();
        reachability_.clear();
        for (QTreeWidgetItem *item : std::as_const(itemById_))
            applyReachability(item);
    }
}

void MainWindow::setExtraColumnVisible(int column, bool visible)
{
    tree_->setColumnHidden(column, !visible);
    if (visible && tree_->header()->sectionSize(column) < 40)
        tree_->header()->resizeSection(column, column == TreeFavoriteColumn ? 40 : 120);
    QSettings settings;
    settings.setValue(QStringLiteral("view/connectionsColumn%1").arg(column), visible);
    settings.setValue(QStringLiteral("view/connections2Header"), tree_->header()->saveState());
}

void MainWindow::setSortByName(bool byName)
{
    sortByName_ = byName;
    QSettings().setValue(QStringLiteral("view/sortByName"), byName);
    rebuildTree(selectedId());
}

void MainWindow::sortStoreByName()
{
    if (locked_)
        return;
    std::function<void(Node &)> sortChildren = [&](Node &node) {
        std::stable_sort(node.children.begin(), node.children.end(),
                         [](const Node &a, const Node &b) { return nodeLessThan(a, b); });
        for (Node &child : node.children)
            sortChildren(child);
    };
    sortChildren(store_.root);
    persist();
    rebuildTree(selectedId());
    statusBar()->showMessage(tr("Archivio riordinato alfabeticamente"), 5000);
}

void MainWindow::syncStoreFromTree()
{
    if (locked_)
        return;
    // Every node by id, without children: the tree widget says where it goes.
    QHash<QString, Node> flat;
    std::function<void(const Node &)> collect = [&](const Node &node) {
        Node copy = node;
        copy.children.clear();
        flat.insert(node.id, copy);
        for (const Node &child : node.children)
            collect(child);
    };
    for (const Node &child : store_.root.children)
        collect(child);

    std::function<QVector<Node>(QTreeWidgetItem *, int)> build = [&](QTreeWidgetItem *parent,
                                                                       int count) {
        QVector<Node> nodes;
        for (int i = 0; i < count; ++i) {
            QTreeWidgetItem *item = parent ? parent->child(i) : tree_->topLevelItem(i);
            const QString id = item->data(0, Qt::UserRole).toString();
            if (!flat.contains(id))
                continue;
            Node node = flat.take(id);
            if (node.kind == Node::Kind::Folder) {
                node.expanded = item->isExpanded();
                node.children = build(item, item->childCount());
            }
            nodes.append(node);
        }
        return nodes;
    };
    QVector<Node> children = build(nullptr, tree_->topLevelItemCount());
    if (!flat.isEmpty())
        return; // the widget lost a node: keep the store as it was
    store_.root.children = children;
    persist();
    rebuildTree(selectedId());
}

void MainWindow::rebuildTree(const QString &selectId)
{
    // Snippets and history live in the same archive: refresh them on
    // unlock, lock and import too.
    rebuildSnippetList();
    rebuildHistoryList();

    // Reachability belongs to the unlocked archive: drop it when locking,
    // check again right after unlocking.
    if (locked_)
        reachability_.clear();
    else if (reachabilityEnabled_ && reachability_.isEmpty())
        QTimer::singleShot(500, this, [this] { checkReachability(); });

    itemById_.clear();
    tree_->clear();
    for (const Node &child : store_.root.children)
        addNodeItem(nullptr, child);
    if (sortByName_)
        tree_->sortItems(0, Qt::AscendingOrder); // NodeItem: folders first, natural order

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

    updateFilterIndicator();
}

void MainWindow::updateFilterIndicator()
{
    if (!filterButton_)
        return;

    // Only the filters hidden behind the menu count: the text filter is
    // already visible in the line edit next to the button.
    QStringList active;
    if (favoritesOnly_)
        active << tr("solo preferiti");
    if (!tagFilter_.isEmpty())
        active << tr("tag: %1").arg(tagFilter_);

    if (active.isEmpty()) {
        filterButton_->setIcon(filterIcon_);
        filterButton_->setToolTip(tr("Filtri connessioni"));
        return;
    }

    const QSize size = filterButton_->iconSize();
    const qreal dpr = filterButton_->devicePixelRatioF();
    QPixmap pixmap = filterIcon_.pixmap(size, dpr);
    {
        const QSizeF logical = pixmap.deviceIndependentSize();
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        const qreal diameter = std::max<qreal>(5.0, logical.width() * 0.45);
        const QRectF badge(logical.width() - diameter, 0, diameter, diameter);
        painter.setPen(QPen(filterButton_->palette().color(QPalette::Window), 1.2));
        painter.setBrush(QColor(0xE5, 0x48, 0x4D));
        painter.drawEllipse(badge.adjusted(0.6, 0.6, -0.6, -0.6));
    }
    filterButton_->setIcon(QIcon(pixmap));
    filterButton_->setToolTip(tr("Filtri connessioni — attivi: %1").arg(active.join(QStringLiteral(", "))));
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

    // Save the command history still waiting for its delayed save.
    if (historySaveTimer_ && historySaveTimer_->isActive()) {
        historySaveTimer_->stop();
        persist();
    }

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
    const QString title = paths::isPortable() ? tr("SSH Manager (portabile)") : tr("SSH Manager");
    setWindowTitle(unlocked ? title : tr("%1 — bloccato").arg(title));

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
    importSshConfigAction_->setEnabled(unlocked);
    exportSshConfigAction_->setEnabled(unlocked);
    generateKeyAction_->setEnabled(unlocked);
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
    bool hasTunnels = false;
    if (isHost) {
        for (const TunnelConfig &tunnel : node->conn.tunnels)
            hasTunnels = hasTunnels || tunnel.enabled;
    }
    startTunnelsAction_->setEnabled(hasTunnels);
    newSnippetAction_->setEnabled(!locked_);
    batchAction_->setEnabled(!locked_);
    healthAction_->setEnabled(isHost);
    copyKeyAction_->setEnabled(isHost);
    updateTransferAction();
    rebuildHistoryList();
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

    // What to export: the hosts selected in the tree, or everything.
    const QStringList selected = selectedHostIds();
    ExportSelectionDialog selection(store_, QSet<QString>(selected.cbegin(), selected.cend()), this);
    if (selection.exec() != QDialog::Accepted)
        return;
    ConnectionStore exported;
    exported.root = archivetransfer::subset(store_.root, selection.hostIds());
    if (!selection.includeCredentials())
        archivetransfer::stripCredentials(exported.root);
    if (selection.includeSnippets())
        exported.snippets = store_.snippets;
    QVector<const Connection *> exportedConnections;
    exported.collectConnections(exportedConnections);
    const int exportedCount = int(exportedConnections.size());

    const QString suggested = QDir::homePath() + QStringLiteral("/sshmanager-export-")
                              + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmm"))
                              + QStringLiteral(".db");
    const QString path =
        QFileDialog::getSaveFileName(this, tr("Esporta archivio"), suggested,
                                     tr("Archivio SSHManager (*.db);;Tutti i file (*)"));
    if (path.isEmpty()) {
        exported.clearSensitive();
        return;
    }
    if (QFileInfo(path).canonicalFilePath() == QFileInfo(vault_.path()).canonicalFilePath()) {
        exported.clearSensitive();
        QMessageBox::warning(this, tr("Esporta archivio"),
                             tr("Scegli un percorso diverso dall'archivio in uso."));
        return;
    }

    MasterPasswordDialog dialog(MasterPasswordDialog::Mode::Create, this);
    dialog.setWindowTitle(tr("Esporta archivio"));
    dialog.setTitleText(tr("Password di esportazione"));
    dialog.setDescriptionText(
        selection.includeCredentials()
            ? tr("Il file esportato conterrà connessioni e credenziali cifrate con questa "
                 "password. Servirà per importarlo, anche su un altro computer.")
            : tr("Il file esportato conterrà le connessioni, senza credenziali, cifrate con "
                 "questa password. Servirà per importarlo, anche su un altro computer."));
    if (dialog.exec() != QDialog::Accepted) {
        exported.clearSensitive();
        return;
    }

    QByteArray password = dialog.password();
    QByteArray payload = QJsonDocument(exported.toJson()).toJson(QJsonDocument::Compact);
    exported.clearSensitive();
    QString error;
    const bool ok = Vault::writeArchive(path, password, payload, &error);
    crypto::cleanse(password);
    crypto::cleanse(payload);
    if (!ok) {
        QMessageBox::critical(this, tr("Esportazione non riuscita"), error);
        return;
    }

    statusBar()->showMessage(tr("%n connessione/i esportata/e in %1", "", exportedCount).arg(path),
                             8000);
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

    ConnectionStore imported;
    QString previewError;
    if (!imported.fromJson(document.object(), &previewError)) {
        imported.clearSensitive();
        QMessageBox::critical(this, tr("Importazione non riuscita"), previewError);
        return;
    }

    // What to import, where, and what to do with the hosts already here.
    ImportSelectionDialog selection(store_, imported, true, this);
    if (selection.exec() != QDialog::Accepted) {
        imported.clearSensitive();
        return;
    }

    if (selection.replaceAll()) {
        QVector<const Connection *> connections;
        imported.collectConnections(connections);
        const int count = int(connections.size());
        imported.clearSensitive();
        if (!store_.fromJson(document.object(), &error)) {
            QMessageBox::critical(this, tr("Importazione non riuscita"), error);
            return;
        }
        if (!persist())
            return;
        rebuildTree();
        updateStatus();
        statusBar()->showMessage(tr("Archivio importato (%1 connessioni)").arg(count), 8000);
        return;
    }

    const archivetransfer::MergeResult result =
        archivetransfer::merge(store_, selection.destinationId(), imported.root,
                               selection.hostIds(), selection.conflict());
    int snippetsAdded = 0;
    if (selection.includeSnippets()) {
        for (const Snippet &snippet : std::as_const(imported.snippets)) {
            const bool present = store_.findSnippet(snippet.id)
                                 || std::any_of(store_.snippets.cbegin(), store_.snippets.cend(),
                                                [&](const Snippet &s) {
                                                    return s.command == snippet.command;
                                                });
            if (!present) {
                store_.snippets.append(snippet);
                ++snippetsAdded;
            }
        }
    }
    imported.clearSensitive();
    if (!persist())
        return;
    rebuildTree();
    rebuildSnippetList();
    updateStatus();

    QStringList parts;
    if (result.added)
        parts << tr("%n aggiunta/e", "", result.added);
    if (result.updated)
        parts << tr("%n aggiornata/e", "", result.updated);
    if (result.copied)
        parts << tr("%n copiata/e", "", result.copied);
    if (result.skipped)
        parts << tr("%n saltata/e", "", result.skipped);
    if (snippetsAdded)
        parts << tr("%n snippet", "", snippetsAdded);
    QMessageBox::information(this, tr("Importa connessioni"),
                             parts.isEmpty() ? tr("Nessuna modifica.")
                                             : tr("Importazione completata: %1.")
                                                   .arg(parts.join(QStringLiteral(", "))));
}

void MainWindow::importSshConfig()
{
    if (locked_ || !vault_.isUnlocked())
        return;
    QString path = openssh::defaultConfigPath();
    if (!QFileInfo::exists(path))
        path = openssh::sshDirectory();
    path = QFileDialog::getOpenFileName(this, tr("Importa da OpenSSH"), path,
                                        tr("Configurazione OpenSSH (config *.conf);;Tutti i file (*)"));
    if (path.isEmpty())
        return;

    QStringList warnings;
    ConnectionStore imported = openssh::importConfig(path, store_, &warnings);
    if (imported.root.children.isEmpty()) {
        QMessageBox::information(this, tr("Importa da OpenSSH"),
                                 tr("Nessun host trovato in %1 (i blocchi con soli caratteri jolly "
                                    "forniscono solo valori predefiniti).")
                                     .arg(QDir::toNativeSeparators(path)));
        return;
    }

    ImportSelectionDialog selection(store_, imported, false, this);
    selection.setWindowTitle(tr("Importa da OpenSSH"));
    if (selection.exec() != QDialog::Accepted)
        return;
    const archivetransfer::MergeResult result =
        archivetransfer::merge(store_, selection.destinationId(), imported.root,
                               selection.hostIds(), selection.conflict());
    if (!persist())
        return;
    rebuildTree();
    updateStatus();

    QStringList parts;
    if (result.added)
        parts << tr("%n aggiunta/e", "", result.added);
    if (result.updated)
        parts << tr("%n aggiornata/e", "", result.updated);
    if (result.copied)
        parts << tr("%n copiata/e", "", result.copied);
    if (result.skipped)
        parts << tr("%n saltata/e", "", result.skipped);
    QString message = parts.isEmpty() ? tr("Nessuna modifica.")
                                      : tr("Importazione completata: %1.")
                                            .arg(parts.join(QStringLiteral(", ")));
    if (!warnings.isEmpty())
        message += QStringLiteral("\n\n") + tr("Da controllare:") + QStringLiteral("\n• ")
                   + warnings.join(QStringLiteral("\n• "));
    QMessageBox::information(this, tr("Importa da OpenSSH"), message);
}

void MainWindow::exportSshConfig()
{
    if (locked_ || !vault_.isUnlocked())
        return;
    QStringList ids = selectedHostIds();
    if (ids.isEmpty()) {
        QVector<const Connection *> all;
        store_.collectConnections(all);
        for (const Connection *connection : std::as_const(all))
            ids << connection->id;
    }
    const QString text = openssh::exportConfig(store_, QSet<QString>(ids.cbegin(), ids.cend()));

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Esporta per OpenSSH"));
    dialog.resize(680, 520);
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(tr("Blocchi da aggiungere a ~/.ssh/config per %n connessione/i. Le "
                                "password non vengono esportate.",
                                "", int(ids.size())),
                             &dialog);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *editor = new QPlainTextEdit(text, &dialog);
    editor->setReadOnly(true);
    editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(editor, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QPushButton *copy = buttons->addButton(tr("Copia"), QDialogButtonBox::ActionRole);
    QPushButton *save = buttons->addButton(tr("Salva…"), QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(copy, &QPushButton::clicked, &dialog, [text, this] {
        QGuiApplication::clipboard()->setText(text);
        statusBar()->showMessage(tr("Configurazione copiata negli appunti"), 5000);
    });
    connect(save, &QPushButton::clicked, &dialog, [&dialog, text, this] {
        const QString file = QFileDialog::getSaveFileName(
            &dialog, tr("Salva configurazione"),
            openssh::sshDirectory() + QStringLiteral("/config_sshmanager"),
            tr("Tutti i file (*)"));
        if (file.isEmpty())
            return;
        if (QFileInfo(file).canonicalFilePath() == QFileInfo(openssh::defaultConfigPath()).canonicalFilePath()
            && QMessageBox::question(&dialog, tr("Salva configurazione"),
                                     tr("Sovrascrivere ~/.ssh/config? In alternativa salva in un "
                                        "file a parte e aggiungi a ~/.ssh/config la riga\n"
                                        "Include config_sshmanager"),
                                     QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
                   != QMessageBox::Yes)
            return;
        QSaveFile out(file);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Text)
            || out.write(text.toUtf8()) < 0 || !out.commit()) {
            QMessageBox::critical(&dialog, tr("Salva configurazione"),
                                  tr("Impossibile scrivere %1.").arg(QDir::toNativeSeparators(file)));
            return;
        }
        statusBar()->showMessage(tr("Configurazione salvata in %1").arg(QDir::toNativeSeparators(file)), 8000);
    });
    layout->addWidget(buttons);
    dialog.exec();
}

void MainWindow::generateKey(const QString &id)
{
    if (locked_ || !vault_.isUnlocked())
        return;
    const Connection *connection = store_.findConnection(id);
    KeyGenDialog dialog(connection ? connection->displayName() : QString(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QString keyPath = dialog.privateKeyPath();

    if (connection && dialog.useForConnection()) {
        const bool copyFirst = dialog.copyToHost();
        // Copy first, with the credentials that work today; then switch.
        if (copyFirst && !copyPublicKey(id, keyPath + QStringLiteral(".pub"), false))
            return; // the connection keeps its current authentication
        if (Connection *target = store_.findConnection(id)) {
            target->auth = AuthType::KeyFile;
            crypto::cleanse(target->privateKey);
            target->keyName.clear();
            target->keyPath = keyPath;
            crypto::cleanse(target->passphrase);
            target->passphrase = dialog.passphrase();
            if (persist())
                rebuildTree(id);
        }
        statusBar()->showMessage(tr("Chiave %1 creata e impostata per %2")
                                     .arg(keyPath, connection ? store_.findConnection(id)->displayName() : QString()),
                                 8000);
        return;
    }
    QMessageBox::information(this, tr("Genera chiave SSH"),
                             tr("Chiave creata:\n%1\n%1.pub").arg(keyPath));
}

bool MainWindow::copyPublicKey(const QString &id, const QString &publicKeyPath, bool offerSwitch)
{
    const Connection *connection = locked_ ? nullptr : store_.findConnection(id);
    if (!connection)
        return false;
    QString suggested = publicKeyPath;
    if (suggested.isEmpty() && connection->auth == AuthType::KeyFile && !connection->keyPath.isEmpty())
        suggested = expandUserPath(connection->keyPath) + QStringLiteral(".pub");

    QString key;
    QString chosenPath = suggested;
    if (publicKeyPath.isEmpty()) {
        PublicKeyDialog dialog(connection->target(), suggested, this);
        if (dialog.exec() != QDialog::Accepted)
            return false;
        key = dialog.key();
        chosenPath = dialog.path();
    } else {
        QString error;
        key = openssh::readPublicKey(publicKeyPath, &error);
        if (key.isEmpty()) {
            QMessageBox::warning(this, tr("Copia la chiave pubblica"), error);
            return false;
        }
    }

    const QString name = connection->displayName();
    SshPlan plan = commandPlan(id);
    if (!plan.valid) {
        for (const QString &path : plan.temporaryFiles)
            QFile::remove(path);
        if (!plan.error.isEmpty())
            QMessageBox::warning(this, tr("Copia la chiave pubblica"), plan.error);
        return false;
    }

    // Wait in a small modal box; the run can be cancelled.
    QMessageBox progress(QMessageBox::NoIcon, tr("Copia la chiave pubblica"),
                         tr("Installazione della chiave su %1…").arg(name), QMessageBox::Cancel, this);
    RemoteRun run(plan, openssh::authorizedKeysCommand(), key.toUtf8() + '\n', 60000);
    RemoteRun::Result result;
    bool finished = false;
    connect(&run, &RemoteRun::finished, &progress, [&](const RemoteRun::Result &r) {
        result = r;
        finished = true;
        progress.done(0);
    });
    run.start();
    if (!finished)
        progress.exec();
    if (!finished) {
        run.cancel();
        QEventLoop wait;
        connect(&run, &RemoteRun::finished, &wait, &QEventLoop::quit);
        if (run.isRunning())
            wait.exec();
    }
    for (const QString &path : plan.temporaryFiles)
        QFile::remove(path);

    const QString out = QString::fromUtf8(result.out);
    if (!result.ok() || !(out.contains(QLatin1String("SSHMANAGER_KEY_ADDED"))
                          || out.contains(QLatin1String("SSHMANAGER_KEY_PRESENT")))) {
        if (result.cancelled)
            return false;
        QString detail = result.startError;
        if (detail.isEmpty())
            detail = QString::fromLocal8Bit(result.err).trimmed();
        if (result.timedOut)
            detail = tr("Tempo scaduto.");
        QMessageBox::critical(this, tr("Copia la chiave pubblica"),
                              tr("Non è stato possibile installare la chiave su %1.\n\n%2")
                                  .arg(name, detail.right(1500)));
        return false;
    }

    const bool present = out.contains(QLatin1String("SSHMANAGER_KEY_PRESENT"));
    QString privatePath = chosenPath;
    if (privatePath.endsWith(QLatin1String(".pub")))
        privatePath.chop(4);
    const Connection *current = store_.findConnection(id);
    const bool canSwitch = offerSwitch && current
                           && !(current->auth == AuthType::KeyFile
                                && expandUserPath(current->keyPath) == expandUserPath(privatePath))
                           && QFileInfo::exists(privatePath);
    const QString done = present ? tr("La chiave era già presente su %1.").arg(name)
                                 : tr("Chiave installata su %1.").arg(name);
    if (!canSwitch) {
        if (offerSwitch)
            QMessageBox::information(this, tr("Copia la chiave pubblica"), done);
        else
            statusBar()->showMessage(done, 8000);
        return true;
    }
    const auto answer = QMessageBox::question(
        this, tr("Copia la chiave pubblica"),
        done + QStringLiteral("\n\n")
            + tr("Usare d'ora in poi questa chiave per accedere a %1?\n%2")
                  .arg(name, QDir::toNativeSeparators(privatePath)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (answer == QMessageBox::Yes) {
        if (Connection *target = store_.findConnection(id)) {
            target->auth = AuthType::KeyFile;
            crypto::cleanse(target->privateKey);
            target->keyName.clear();
            target->keyPath = QDir::toNativeSeparators(privatePath);
            if (persist())
                rebuildTree(id);
        }
    }
    return true;
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
    // Window geometry commands make no sense when every session is a tab.
    maximizeSessionAction_->setEnabled(session != nullptr && !tabbedView_);
    restoreSessionAction_->setEnabled(session != nullptr && !tabbedView_);
    nextSessionAction_->setEnabled(sessions_.size() > 1);
    previousSessionAction_->setEnabled(sessions_.size() > 1);
    const bool hasSessions = !locked_ && !sessions_.isEmpty();
    closeAllAction_->setEnabled(hasSessions);
    const bool canArrange = hasSessions && !tabbedView_;
    tileAction_->setEnabled(canArrange);
    tileVerticalAction_->setEnabled(canArrange);
    tileHorizontalAction_->setEnabled(canArrange);
    gridAction_->setEnabled(canArrange);
    cascadeAction_->setEnabled(canArrange);
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

    // Not modal: terminals stay usable while the queue works. The window owns
    // the plan's temporary files and deletes them when it closes.
    auto *dialog = new FileTransferDialog(effective, plan, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &FileTransferDialog::remoteBookmarksChanged, this,
            [this](const QString &connectionId, const QStringList &bookmarks) {
                if (locked_)
                    return;
                if (Connection *connection = store_.findConnection(connectionId)) {
                    connection->remoteBookmarks = bookmarks;
                    persist();
                }
            });
    dialog->show();
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

void MainWindow::runExternalTool(const ExternalTool &tool, const QString &id)
{
    if (locked_)
        return;
    const Connection *stored = store_.findConnection(id);
    if (!stored)
        return;
    Connection connection = *stored;
    connection.clearSensitive(); // tools never see credentials

    // Custom ${variables} are asked once per launch.
    QHash<QString, QString> custom;
    const QStringList variables = snippets::customVariables(tool.command);
    if (!variables.isEmpty()) {
        if (!askSnippetVariables(this, tool.name, variables, &snippetValues_))
            return;
        custom = snippetValues_;
    }

    if (tool.mode == ExternalTool::Mode::Url) {
        const QString address =
            snippets::expandBuiltins(snippets::applyVariables(tool.command, custom), connection);
        if (!QDesktopServices::openUrl(QUrl::fromUserInput(address)))
            QMessageBox::warning(this, tool.name, tr("Impossibile aprire %1").arg(address));
        return;
    }

    QStringList tokens = externaltools::expandCommand(tool.command, connection, custom);
    if (tokens.isEmpty())
        return;

    if (tool.mode == ExternalTool::Mode::Detached) {
        const QString program = tokens.takeFirst();
        if (!QProcess::startDetached(program, tokens))
            QMessageBox::warning(this, tool.name, tr("Impossibile avviare %1").arg(program));
        else
            statusBar()->showMessage(tr("%1 avviato per %2").arg(tool.name, connection.displayName()), 5000);
        return;
    }

    // In a terminal tab of its own, like a local shell.
    Connection local;
    local.id = ConnectionStore::newId();
    local.local = true;
    local.name = tr("%1 — %2").arg(tool.name, connection.displayName());
    local.host = connection.host;
    local.port = connection.port;
    local.user = connection.user;
    local.auth = AuthType::Agent;
    local.terminal = connection.terminal;

    SshPlan plan;
    plan.valid = true;
    if (QFileInfo(tokens.first()).isAbsolute() && QFileInfo::exists(tokens.first())) {
        plan.program = tokens.takeFirst();
        plan.args = tokens;
    } else {
        // Not a program on the PATH (a shell built-in, a script...): let the
        // shell run it.
#ifdef Q_OS_WIN
        plan.program = qEnvironmentVariable("COMSPEC", QStringLiteral("cmd.exe"));
        plan.args = QStringList{QStringLiteral("/c")} + tokens;
#else
        plan.program = QStringLiteral("/bin/sh");
        QStringList quoted;
        for (const QString &token : std::as_const(tokens))
            quoted << QLatin1Char('\'') + QString(token).replace(QLatin1Char('\''), QStringLiteral("'\\''"))
                          + QLatin1Char('\'');
        plan.args = QStringList{QStringLiteral("-c"), quoted.join(QLatin1Char(' '))};
#endif
    }
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
        previousSessionAction_, nextSessionAction_, externalToolsAction_,
        importSshConfigAction_, exportSshConfigAction_, generateKeyAction_, copyKeyAction_,
        aboutAction_,
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
    connect(session, &SessionView::hostKeyChanged, this,
            [this](SessionView *from, const QString &hostName, const QString &fingerprint) {
                handleHostKeyChanged(hostName, fingerprint, from->connection().id, from, nullptr);
            });
    connect(session, &SessionView::commandEntered, this, &MainWindow::recordCommand);
    connect(session, &SessionView::sessionEnded, this, &MainWindow::handleSessionEnded);
    connect(session, &SessionView::attentionChanged, this, &MainWindow::updateSessionAttention);
    connect(session, &SessionView::bellRang, this, [this](SessionView *) {
        if (!isActiveWindow())
            QApplication::alert(this); // flash in the taskbar
    });
    connect(session, &SessionView::quickConnectRequested, this, &MainWindow::connectQuick);
    connect(session, &SessionView::reconnectRequested, this, &MainWindow::reconnectSession);
    connect(session, &SessionView::outputSettled, this, [this](SessionView *from) {
        const QString title = tr("Output fermo in %1").arg(from->connection().displayName());
        statusBar()->showMessage(title, 8000);
        notify(title, tr("Il terminale non produce output da 10 secondi."), QSystemTrayIcon::Information);
    });
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
    if (tabbedView_) {
        // showMaximized() on a hidden subwindow sets the state but keeps the
        // initial size: show it first, then maximize again once the tab
        // and the area layout have settled.
        sub->setGeometry(mdi_->viewport()->rect());
        sub->show();
        sub->showMaximized();
        QTimer::singleShot(0, sub, [this, sub] {
            if (tabbedView_)
                sub->showMaximized();
        });
    } else {
        sub->resize(940, 580);
        sub->show();
    }

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

    // Name plus an explicit position: the selection only suggests the parent,
    // so a top-level folder is one choice away whatever is selected.
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Nuova cartella"));
    auto *form = new QFormLayout(&dialog);
    auto *nameEdit = new QLineEdit(tr("Nuovo gruppo %1").arg(unnamedCounter_ + 1), &dialog);
    nameEdit->selectAll();
    nameEdit->setMinimumWidth(300);
    auto *position = new QComboBox(&dialog);
    position->addItem(style()->standardIcon(QStyle::SP_DirHomeIcon), tr("Livello principale"),
                      store_.root.id);
    std::function<void(const Node &, const QString &)> addFolders = [&](const Node &node,
                                                                       const QString &prefix) {
        for (const Node &child : node.children) {
            if (child.kind != Node::Kind::Folder)
                continue;
            const QString label = prefix.isEmpty() ? child.name : prefix + QStringLiteral(" / ") + child.name;
            position->addItem(style()->standardIcon(QStyle::SP_DirIcon), label, child.id);
            addFolders(child, label);
        }
    };
    addFolders(store_.root, QString());
    position->setCurrentIndex(qMax(0, position->findData(targetParent)));
    form->addRow(tr("Nome"), nameEdit);
    form->addRow(tr("Posizione"), position);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted || nameEdit->text().trimmed().isEmpty())
        return;
    ++unnamedCounter_;
    targetParent = position->currentData().toString();

    Node folder;
    folder.kind = Node::Kind::Folder;
    folder.id = ConnectionStore::newId();
    folder.name = nameEdit->text().trimmed();

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

void MainWindow::setupResizableHeader(QTreeWidget *view, const QString &key, int firstColumnWidth)
{
    // Every column can be dragged; the last one fills the remaining width, and
    // a horizontal scroll bar appears when the columns are wider than the dock.
    QHeaderView *header = view->header();
    header->setSectionsMovable(false);
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setStretchLastSection(true);
    header->setMinimumSectionSize(40);
    header->resizeSection(0, firstColumnWidth);
    view->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    view->setTextElideMode(Qt::ElideMiddle);

    const QString settingsKey = QStringLiteral("view/%1Header").arg(key);
    QSettings settings;
    const QByteArray state = settings.value(settingsKey).toByteArray();
    if (!state.isEmpty())
        header->restoreState(state);

    // Double-clicking a column divider fits that column to its contents.
    connect(header, &QHeaderView::sectionHandleDoubleClicked, view,
            [view](int section) { view->resizeColumnToContents(section); });
    connect(header, &QHeaderView::sectionResized, this, [header, settingsKey] {
        QSettings().setValue(settingsKey, header->saveState());
    });
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // With background tunnels running, closing the window keeps the app in
    // the tray; "Esci" (tray menu or Ctrl+Q) really quits.
    saveLayout();
    if (!quitting_ && tray_ && tray_->isVisible() && keepInTray_ && !tunnelServices_.isEmpty()) {
        persist();
        hide();
        event->ignore();
        if (!trayHintShown_) {
            trayHintShown_ = true;
            tray_->showMessage(tr("SSH Manager è ancora attivo"),
                               tr("I tunnel in background continuano a funzionare. "
                                  "Usa Esci dal menu della tray per chiudere l'app."),
                               QSystemTrayIcon::Information, 6000);
        }
        return;
    }

    persist();

    // Background tunnels end with the application; their destructors kill
    // ssh and delete the key/askpass files.
    const QList<TunnelService *> services = tunnelServices_;
    tunnelServices_.clear();
    serviceStates_.clear();
    droppedServices_.clear();
    for (TunnelService *service : services)
        service->disconnect(this);
    qDeleteAll(services);
    if (tray_)
        tray_->hide();

    const auto windows = mdi_->subWindowList();
    for (QMdiSubWindow *sub : windows)
        sub->close();

    store_.clearSensitive();
    vault_.lock();

    QMainWindow::closeEvent(event);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // A drop in the connection tree has just moved items: once the widget
    // has done it, write the new structure to the archive.
    if (tree_ && watched == tree_->viewport() && event->type() == QEvent::Drop && !locked_)
        QTimer::singleShot(0, this, &MainWindow::syncStoreFromTree);

    // A click on the empty part of the connection tree drops the selection,
    // so that "new folder/connection" go to the top level.
    if (tree_ && watched == tree_->viewport() && event->type() == QEvent::MouseButtonPress) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (!tree_->itemAt(mouse->position().toPoint())) {
            tree_->clearSelection();
            tree_->setCurrentItem(nullptr);
            updateNodeActions();
        }
    }

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
