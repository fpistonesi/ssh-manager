#pragma once

#include "externaltools.h"
#include "hostprobe.h"
#include "model.h"
#include "sshcommand.h"
#include "vault.h"

#include <QSystemTrayIcon>

#include <QHash>
#include <QIcon>
#include <QSet>
#include <QList>
#include <QMainWindow>

class QAction;
class QDockWidget;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QMdiArea;
class QMdiSubWindow;
class QMenu;
class QSystemTrayIcon;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class SessionView;
class TunnelService;
struct SshPlan;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class SessionLayout { Vertical, Horizontal, Grid };

    void createActions();
    void createDocks();
    QWidget *createRibbon(QWidget *parent);
    void setupResizableHeader(QTreeWidget *view, const QString &key, int firstColumnWidth);

    void rebuildTree(const QString &selectId = QString());
    void setSortByName(bool byName);
    void sortStoreByName();
    void syncStoreFromTree();
    QTreeWidgetItem *addNodeItem(QTreeWidgetItem *parentItem, const Node &node);
    void applyFilter();
    void updateFilterIndicator();
    void updateStatus();
    bool persist();

    void initializeVault();
    void createVault();
    void requestUnlock();
    void finishUnlock(QByteArray payload);
    void lockVault();
    void changeMasterPassword();
    void setAutoLockMinutes(int minutes);
    void cycleSession(int direction);
    void setTabbedView(bool tabbed);
    void arrangeSessions(SessionLayout layout);
    void customizeActiveTerminal();
    void exportArchive();
    void importArchive();
    void backupArchive();
    void importSshConfig();
    void exportSshConfig();
    void generateKey(const QString &id);
    // `publicKeyPath` empty: ask which key. Returns true when the key is on
    // the host. `offerSwitch`: then propose to log in with that key.
    bool copyPublicKey(const QString &id, const QString &publicKeyPath = QString(),
                       bool offerSwitch = true);
    void applyLockedState();
    void armAutoLock();
    void loadSettings();

    bool resolveConnection(const QString &id, Connection *effective, QVector<Connection> *chain);
    void openConnection(const QString &id);
    void openLocalTerminal();
    void showQuickPalette();
    void connectQuick(const QString &target);
    void rememberRecentConnection(const QString &id);
    void rememberQuickTarget(const QString &target);
    void transferFiles();
    void startSession(const Connection &connection, const SshPlan &plan);
    void newConnection(const QString &folderId);
    void newFolder(const QString &parentId);
    void editConnection(const QString &id);
    void renameNode(const QString &id);
    void deleteNode(const QString &id);
    void duplicateConnection(const QString &id);

    void forwardKey(SessionView *from, QKeyEvent *event);
    void forwardPaste(SessionView *from, const QString &text);
    void updateBroadcastState();
    void updateSessionAction();
    void updateNodeActions();
    void updateTransferAction();
    void toggleFavorite(bool favorite);
    void rebuildSessionList();
    void createCommandDocks();
    void rebuildSnippetList();
    void rebuildHistoryList();
    void recordCommand(SessionView *session, const QString &command);
    // Sends text to the active session, or to every session included in the
    // broadcast; asks custom ${variables} first.
    void sendCommand(const QString &text, bool execute, bool toBroadcast, const QString &title);
    void newSnippet(const Snippet &prefill);
    void editSnippet(const QString &id);
    void rebuildTunnelList();
    void refreshTunnelStatus();
    SessionView *tunnelRowSession(QTreeWidgetItem *item) const;
    TunnelService *tunnelRowService(QTreeWidgetItem *item) const;
    TunnelService *tunnelServiceFor(const QString &connectionId) const;
    void startTunnelService(const QString &id);
    void stopTunnelService(TunnelService *service);
    void handleTunnelServiceState(TunnelService *service);
    void handleHostKeyChanged(const QString &hostName, const QString &newFingerprint,
                              const QString &connectionId, SessionView *session,
                              TunnelService *service);
    void showKnownHosts(const QString &filter = QString());

    void saveLayout();
    void resetLayout();
    void applyAppStyle();
    QIcon layoutIcon(int kind) const;
    void refreshThemedIcons();
    void setTheme(const QString &theme);
    void setLanguage(const QString &language);
    void refreshThemeDependentUi();
    void updateSessionAttention(SessionView *session);

    SshPlan commandPlan(const QString &id);
    QStringList selectedHostIds() const;
    void runBatch();
    void showHostHealth(const QString &id);
    void runExternalTool(const ExternalTool &tool, const QString &id);
    void setExtraColumnVisible(int column, bool visible);

    void handleSessionEnded(SessionView *session, const QString &reason, bool unexpected);
    void reconnectSession(SessionView *old);
    void notify(const QString &title, const QString &body, QSystemTrayIcon::MessageIcon icon);
    void applyReachability(QTreeWidgetItem *item);
    void checkReachability(const QString &onlyId = QString());
    void setReachabilityEnabled(bool enabled);

    void createTray();
    void updateTray();
    void rebuildTrayMenu();
    void showFromTray();
    void quitApplication();
    void highlightActiveSession();
    void activateSession(SessionView *session);

    QString selectedId() const;
    SessionView *activeSession() const;
    void showAbout();

    ConnectionStore store_;
    Vault vault_;

    QMdiArea *mdi_ = nullptr;
    QDockWidget *connectionsDock_ = nullptr;
    QTreeWidget *tree_ = nullptr;
    QTreeWidget *sessionList_ = nullptr;
    QDockWidget *sessionDock_ = nullptr;
    QDockWidget *tunnelDock_ = nullptr;
    QTreeWidget *tunnelList_ = nullptr;
    QTimer *tunnelTimer_ = nullptr;
    QList<TunnelService *> tunnelServices_;
    QAction *startTunnelsAction_ = nullptr;
    QAction *knownHostsAction_ = nullptr;
    QAction *newSnippetAction_ = nullptr;
    QAction *resetLayoutAction_ = nullptr;
    QList<QAction *> themeActions_;
    QList<QAction *> languageActions_;
    QToolButton *themeButton_ = nullptr;
    QToolButton *languageButton_ = nullptr;
    QAction *batchAction_ = nullptr;
    QAction *healthAction_ = nullptr;
    QAction *externalToolsAction_ = nullptr;
    QAction *reachabilityAction_ = nullptr;
    QAction *checkReachabilityAction_ = nullptr;
    HostProbe *probe_ = nullptr;
    QTimer *reachabilityTimer_ = nullptr;
    QHash<QString, HostProbe::Result> reachability_; // by connection id
    bool reachabilityEnabled_ = false;
    static constexpr int kReachabilityMinutes = 5;
    // Bump when the set of docks changes in a way old layouts cannot handle.
    static constexpr int kLayoutVersion = 1;
    QByteArray defaultLayout_;
    bool defaultLayoutCaptured_ = false;
    QDockWidget *snippetDock_ = nullptr;
    QTreeWidget *snippetList_ = nullptr;
    QLineEdit *snippetFilter_ = nullptr;
    QDockWidget *historyDock_ = nullptr;
    QTreeWidget *historyList_ = nullptr;
    QString historyConnectionId_;
    QTimer *historySaveTimer_ = nullptr;
    QHash<QString, QString> snippetValues_; // last values of custom variables
    // Last state seen per service, to notify transitions (drop, recovery).
    QHash<TunnelService *, int> serviceStates_;
    QSet<TunnelService *> droppedServices_;

    QSystemTrayIcon *tray_ = nullptr;
    QMenu *trayMenu_ = nullptr;
    QIcon appIcon_;
    bool keepInTray_ = true;
    bool trayHintShown_ = false;
    bool quitting_ = false;
    QLineEdit *filter_ = nullptr;
    QToolButton *filterButton_ = nullptr;
    QIcon filterIcon_;
    QLabel *statusInfo_ = nullptr;
    QHash<QString, QTreeWidgetItem *> itemById_;
    QList<SessionView *> sessions_;

    QAction *unlockAction_ = nullptr;
    QAction *lockAction_ = nullptr;
    QAction *changePasswordAction_ = nullptr;
    QAction *newConnectionAction_ = nullptr;
    QAction *newFolderAction_ = nullptr;
    QAction *connectAction_ = nullptr;
    QAction *localTerminalAction_ = nullptr;
    QAction *quickAction_ = nullptr;
    QAction *editAction_ = nullptr;
    QAction *deleteAction_ = nullptr;
    QAction *duplicateAction_ = nullptr;
    QAction *renameAction_ = nullptr;
    QAction *saveAction_ = nullptr;
    QAction *exportAction_ = nullptr;
    QAction *importAction_ = nullptr;
    QAction *backupAction_ = nullptr;
    QAction *importSshConfigAction_ = nullptr;
    QAction *exportSshConfigAction_ = nullptr;
    QAction *generateKeyAction_ = nullptr;
    QAction *copyKeyAction_ = nullptr;
    QAction *quitAction_ = nullptr;
    QAction *broadcastAction_ = nullptr;
    QAction *includeAction_ = nullptr;
    QAction *terminalAction_ = nullptr;
    QAction *transferAction_ = nullptr;
    QAction *favoriteAction_ = nullptr;
    QAction *favoritesOnlyAction_ = nullptr;
    QAction *closeSessionAction_ = nullptr;
    QAction *closeAllAction_ = nullptr;
    QAction *tileAction_ = nullptr;
    QAction *tileVerticalAction_ = nullptr;
    QAction *tileHorizontalAction_ = nullptr;
    QAction *gridAction_ = nullptr;
    QAction *cascadeAction_ = nullptr;
    QAction *tabbedViewAction_ = nullptr;
    QAction *maximizeSessionAction_ = nullptr;
    QAction *restoreSessionAction_ = nullptr;
    QAction *previousSessionAction_ = nullptr;
    QAction *nextSessionAction_ = nullptr;
    QAction *aboutAction_ = nullptr;

    QTimer *autoLockTimer_ = nullptr;
    int autoLockMinutes_ = 15;
    bool favoritesOnly_ = false;
    bool tabbedView_ = false;
    bool sortByName_ = false; // alphabetical view instead of the stored order
    QString tagFilter_;
    bool locked_ = true;
    int unnamedCounter_ = 0;
};
