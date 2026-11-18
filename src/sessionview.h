#pragma once

#include "model.h"
#include "sshcommand.h"

#include <QWidget>

class QKeyEvent;
class QLabel;
class QTermWidget;

// One MDI session: a QTermWidget running the ssh client for a saved
// connection, with a small status strip and broadcast support.
class SessionView : public QWidget {
    Q_OBJECT

public:
    SessionView(const Connection &connection, const SshPlan &plan, QWidget *parent = nullptr);
    ~SessionView() override;

    const Connection &connection() const { return connection_; }
    QString title() const;
    QTermWidget *terminal() const { return terminal_; }

    bool isFinished() const { return finished_; }
    bool isIncludedInBroadcast() const { return included_; }
    void setIncludedInBroadcast(bool included);
    void setBroadcastActive(bool active);

    void sendKeyEvent(QKeyEvent *event);
    void sendText(const QString &text);
    void start();
    void applyTerminalProfile(const TerminalProfile &profile);

signals:
    void keyPressed(SessionView *self, QKeyEvent *event);
    void pasteRequested(SessionView *self, const QString &text);
    void stateChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void refreshStatus();
    void cleanupFiles();
    void terminateSession();

    Connection connection_;
    SshPlan plan_;
    QTermWidget *terminal_ = nullptr;
    QLabel *status_ = nullptr;
    bool included_ = true;
    bool broadcastActive_ = false;
    bool started_ = false;
    bool finished_ = false;
};
