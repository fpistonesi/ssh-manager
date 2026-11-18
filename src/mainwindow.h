#pragma once

#include "model.h"
#include "vault.h"

#include <QHash>
#include <QList>
#include <QMainWindow>

class QAction;
class QDockWidget;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QMdiArea;
class QMdiSubWindow;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class SessionView;
struct SshPlan;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class SessionLayout { Vertical, Horizontal, Grid };

    void createActions();
    void createDocks();
    QWidget *createRibbon(QWidget *parent);

    void rebuildTree(const QString &selectId = QString());
    QTreeWidgetItem *addNodeItem(QTreeWidgetItem *parentItem, const Node &node);
    void applyFilter();
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
    void arrangeSessions(SessionLayout layout);
    void customizeActiveTerminal();
    void exportArchive();
    void importArchive();
    void backupArchive();
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
    QLineEdit *filter_ = nullptr;
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
    QAction *maximizeSessionAction_ = nullptr;
    QAction *restoreSessionAction_ = nullptr;
    QAction *previousSessionAction_ = nullptr;
    QAction *nextSessionAction_ = nullptr;
    QAction *aboutAction_ = nullptr;

    QTimer *autoLockTimer_ = nullptr;
    int autoLockMinutes_ = 15;
    bool favoritesOnly_ = false;
    QString tagFilter_;
    bool locked_ = true;
    int unnamedCounter_ = 0;
};
