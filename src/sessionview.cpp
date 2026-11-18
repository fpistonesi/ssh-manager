#include "sessionview.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QFile>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QLabel>
#include <QVBoxLayout>

#include <qtermwidget.h>

#include <csignal>

SessionView::SessionView(const Connection &connection, const SshPlan &plan, QWidget *parent)
    : QWidget(parent), connection_(connection), plan_(plan)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    terminal_ = new QTermWidget(0, this);
    terminal_->setShellProgram(plan_.program);
    terminal_->setArgs(plan_.args);
    terminal_->setEnvironment(plan_.environment);

    applyTerminalProfile(connection_.terminal);
    terminal_->setScrollBarPosition(QTermWidget::ScrollBarRight);
    terminal_->setFlowControlEnabled(false);
    terminal_->setConfirmMultilinePaste(false);
    terminal_->setTrimPastedTrailingNewlines(true);
    terminal_->setAutoClose(true);

    layout->addWidget(terminal_, 1);

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("sessionStatus"));
    status_->setMargin(4);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(status_, 0);

    terminal_->installEventFilter(this);
    if (QWidget *proxy = terminal_->focusProxy())
        proxy->installEventFilter(this);

    connect(terminal_, &QTermWidget::finished, this, [this] {
        finished_ = true;
        refreshStatus();
    });

    refreshStatus();
}

SessionView::~SessionView()
{
    terminateSession();
    cleanupFiles();
    // The session copy is sanitized by the caller; wipe it anyway.
    connection_.clearSensitive();
}

void SessionView::applyTerminalProfile(const TerminalProfile &profile)
{
    connection_.terminal = profile;

    QFont font = profile.fontFamily.isEmpty()
                     ? QFontDatabase::systemFont(QFontDatabase::FixedFont)
                     : QFont(profile.fontFamily);
    font.setPointSize(qBound(6, profile.fontSize, 48));

    terminal_->setTerminalFont(font);
    terminal_->setColorScheme(profile.colorScheme);
    terminal_->setHistorySize(qBound(100, profile.historySize, 200000));
    terminal_->setKeyboardCursorShape(
        static_cast<QTermWidget::KeyboardCursorShape>(qBound(0, profile.cursorShape, 2)));
    terminal_->setBlinkingCursor(profile.blinkCursor);
    terminal_->setTerminalOpacity(qBound(20, profile.opacityPercent, 100) / 100.0);
    terminal_->setMargin(qBound(0, profile.margin, 24));
}

QString SessionView::title() const
{
    if (connection_.local)
        return connection_.displayName();
    return connection_.displayName() + QStringLiteral(" — ") + connection_.target();
}

void SessionView::refreshStatus()
{
    int tunnels = 0;
    for (const TunnelConfig &tunnel : connection_.tunnels) {
        if (tunnel.enabled)
            ++tunnels;
    }

    QString text = connection_.local ? tr("sessione locale") : connection_.target();
    if (tunnels > 0)
        text += QStringLiteral(" · %1 tunnel").arg(tunnels);
    if (finished_)
        text += QStringLiteral(" · sessione terminata");
    else if (started_)
        text += QStringLiteral(" · attiva");
    if (broadcastActive_ && included_)
        text += QStringLiteral(" · TX");
    status_->setText(text);
    emit stateChanged();
}

void SessionView::setIncludedInBroadcast(bool included)
{
    included_ = included;
    refreshStatus();
}

void SessionView::setBroadcastActive(bool active)
{
    broadcastActive_ = active;
    refreshStatus();
}

void SessionView::start()
{
    if (started_)
        return;
    started_ = true;
    terminal_->startShellProgram();
    terminal_->setFocus();
    refreshStatus();
}

void SessionView::sendKeyEvent(QKeyEvent *event)
{
    if (!terminal_ || !event)
        return;
    QKeyEvent copy(event->type(), event->key(), event->modifiers(), event->text(),
                   event->isAutoRepeat(), event->count());
    terminal_->sendKeyEvent(&copy);
}

void SessionView::sendText(const QString &text)
{
    if (!terminal_)
        return;
    QString normalized = text;
    normalized.replace(QLatin1String("\r\n"), QLatin1String("\r"));
    normalized.replace(QLatin1Char('\n'), QLatin1Char('\r'));
    terminal_->sendText(normalized);
}

bool SessionView::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        const bool ctrlShift =
            key->modifiers().testFlag(Qt::ControlModifier)
            && key->modifiers().testFlag(Qt::ShiftModifier);

        if (ctrlShift) {
            // Ctrl+Shift+V is qtermwidget's paste: broadcast the clipboard text
            // instead of the shortcut itself.
            if (key->key() == Qt::Key_V) {
                const QString text = QGuiApplication::clipboard()->text();
                if (!text.isEmpty())
                    emit pasteRequested(this, text);
            }
        } else if (!key->modifiers().testFlag(Qt::MetaModifier)) {
            emit keyPressed(this, key);
        }
    }
    return QWidget::eventFilter(watched, event);
}

void SessionView::closeEvent(QCloseEvent *event)
{
    terminateSession();
    cleanupFiles();
    QWidget::closeEvent(event);
}

void SessionView::terminateSession()
{
    if (!terminal_)
        return;
    const int pid = terminal_->getShellPID();
    if (pid > 0)
        ::kill(pid, SIGHUP);
}

void SessionView::cleanupFiles()
{
    for (const QString &path : plan_.temporaryFiles)
        QFile::remove(path);
    plan_.temporaryFiles.clear();
}
