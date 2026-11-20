#pragma once

#include "model.h"
#include "sessionend.h"
#include "sshcommand.h"

#include <QElapsedTimer>
#include <QWidget>

class QFrame;
class QKeyEvent;
class QLabel;
class QTermWidget;

// One MDI session: a QTermWidget running the ssh client for a saved
// connection, with a small status strip and broadcast support.
class SessionView : public QWidget {
    Q_OBJECT

public:
    // Something happened in a session the user is not looking at.
    enum class Attention { None, Activity, Bell };

    SessionView(const Connection &connection, const SshPlan &plan, QWidget *parent = nullptr);
    ~SessionView() override;

    const Connection &connection() const { return connection_; }
    QString title() const;
    QTermWidget *terminal() const { return terminal_; }

    bool isFinished() const { return finished_; }
    const SshPlan &plan() const { return plan_; }
    QString endReason() const { return endReason_.text; }
    Attention attention() const { return attention_; }
    // The window shows this session (clears the attention marker).
    void setActive(bool active);
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
    // The program ended; `unexpected` when it was not a normal logout.
    void sessionEnded(SessionView *self, const QString &reason, bool unexpected);
    void reconnectRequested(SessionView *self);
    void attentionChanged(SessionView *self);
    void bellRang(SessionView *self);
    // "Quick connection to <selected host>" from the context menu.
    void quickConnectRequested(const QString &target);
    // "Notify me when the output stops" fired.
    void outputSettled(SessionView *self);
    // A command line was submitted at a shell prompt (read from the screen,
    // so text the terminal does not echo, like passwords, never shows up).
    void commandEntered(SessionView *self, const QString &command);
    // ssh refused the connection because the stored key of `hostName`
    // ("host" or "[host]:port", possibly a jump host) no longer matches.
    // `newFingerprint` is the key the server sent, when ssh printed it.
    void hostKeyChanged(SessionView *self, const QString &hostName, const QString &newFingerprint);
    void stateChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void refreshStatus();
    void showEndBar(bool exitKnown, int exitCode);
    void setNotifyOnSilence(bool enabled);
    void setAttention(Attention attention);
    static bool copyOnSelect();
    void copySelection();
    void pasteClipboard();
    void showContextMenu(const QPoint &pos);
    void clearTerminal();
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
    QString recentOutput_; // tail of the output, to explain how the session ended
    sessionend::Reason endReason_;
    QElapsedTimer uptime_;
    QFrame *endBar_ = nullptr;
    QLabel *endIcon_ = nullptr;
    QLabel *endLabel_ = nullptr;
    bool notifyOnSilence_ = false;
    bool sawOutput_ = false;
    bool terminating_ = false;
    bool active_ = true;
    Attention attention_ = Attention::None;
#ifdef _WIN32
    bool clearPending_ = false;
#endif
};
