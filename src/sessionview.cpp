#include "sessionview.h"

#include "knownhosts.h"
#include "sessionend.h"
#include "snippets.h"

#include <QDesktopServices>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QSettings>
#include <QStyleHints>
#include <QUrl>
#include <QMdiSubWindow>
#include <QPushButton>
#include <QRegularExpression>
#include <QStyle>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QFile>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QTimer>
#include <QVBoxLayout>

#include <qtermwidget.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#endif

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
#ifdef SSHMANAGER_QTW_EXIT_INFO
    // The end of the session is reported by the bar below, with a reason.
    terminal_->setExitDialogEnabled(false);
#endif

    // Shown when the session ends: reason, exit code, Reconnect / Close.
    endBar_ = new QFrame(this);
    endBar_->setObjectName(QStringLiteral("sessionEndBar"));
    endBar_->setFrameShape(QFrame::StyledPanel);
    auto *endLayout = new QHBoxLayout(endBar_);
    endLayout->setContentsMargins(8, 4, 4, 4);
    endIcon_ = new QLabel(endBar_);
    endLabel_ = new QLabel(endBar_);
    endLabel_->setWordWrap(true);
    endLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *reconnect = new QPushButton(
        QIcon::fromTheme(QStringLiteral("view-refresh"), style()->standardIcon(QStyle::SP_BrowserReload)),
        tr("Riconnetti"), endBar_);
    reconnect->setToolTip(tr("Apri di nuovo la connessione in questa posizione"));
    auto *closeSession = new QPushButton(tr("Chiudi"), endBar_);
    endLayout->addWidget(endIcon_);
    endLayout->addWidget(endLabel_, 1);
    endLayout->addWidget(reconnect);
    endLayout->addWidget(closeSession);
    endBar_->hide();
    layout->addWidget(endBar_, 0);
    connect(reconnect, &QPushButton::clicked, this, [this] { emit reconnectRequested(this); });
    connect(closeSession, &QPushButton::clicked, this, [this] {
        for (QWidget *w = parentWidget(); w; w = w->parentWidget()) {
            if (auto *sub = qobject_cast<QMdiSubWindow *>(w)) {
                sub->close();
                return;
            }
        }
    });

    layout->addWidget(terminal_, 1);

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("sessionStatus"));
    status_->setMargin(4);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(status_, 0);

    terminal_->installEventFilter(this);
    if (QWidget *proxy = terminal_->focusProxy())
        proxy->installEventFilter(this);

    // qtermwidget has no context menu of its own: right clicks on the display
    // propagate to the QTermWidget, which reports them here.
    terminal_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(terminal_, &QWidget::customContextMenuRequested, this, &SessionView::showContextMenu);

    // Keep the last few KB of output: when ssh stops on a changed host key,
    // the main window explains it instead of leaving only ssh's warning.
    // Links: Ctrl+click opens them (a plain click stays a selection); the
    // context menu's "Open link" always does.
    connect(terminal_, &QTermWidget::urlActivated, this, [](const QUrl &url, bool fromContextMenu) {
        if (fromContextMenu || QGuiApplication::keyboardModifiers().testFlag(Qt::ControlModifier))
            QDesktopServices::openUrl(url);
    });
    // Bell: always reported, so that the window can flash in the taskbar.
    connect(terminal_, &QTermWidget::bell, this, [this](const QString &) {
        if (!active_)
            setAttention(Attention::Bell);
        emit bellRang(this);
    });

    connect(terminal_, &QTermWidget::receivedData, this, [this](const QString &text) {
        if (finished_)
            return;
        // Output in a session the user is not looking at.
        if (!active_ && attention_ == Attention::None && started_)
            setAttention(Attention::Activity);
        sawOutput_ = true;
        recentOutput_ += text;
        if (recentOutput_.size() > 8192)
            recentOutput_.remove(0, recentOutput_.size() - 8192);
    });
    // "Notify me when the output stops": after some output, 10 s of silence.
    connect(terminal_, &QTermWidget::silence, this, [this] {
        if (!notifyOnSilence_ || !sawOutput_ || finished_)
            return;
        setNotifyOnSilence(false);
        emit outputSettled(this);
    });
    connect(terminal_, &QTermWidget::finished, this, [this] {
        finished_ = true;
        refreshStatus();
        setNotifyOnSilence(false);
        // Drop terminal control sequences the pseudo console mixes in.
        const QString output = sessionend::plainText(recentOutput_);

        bool exitKnown = false;
        int exitCode = 0;
        bool crashed = false;
#ifdef SSHMANAGER_QTW_EXIT_INFO
        exitKnown = true;
        exitCode = terminal_->exitCode();
        crashed = terminal_->exitCrashed() && !terminating_;
#endif
        endReason_ = sessionend::describe(exitKnown, exitCode, crashed, output, connection_.local);
        showEndBar(exitKnown, exitCode);
        emit sessionEnded(this, endReason_.text, endReason_.unexpected);

        if (output.contains(QLatin1String("REMOTE HOST IDENTIFICATION HAS CHANGED"))) {
            // "Host key for <name> has changed and you have requested strict
            // checking." names the host (a jump host too); the terminal may
            // have wrapped the line, so match across whitespace.
            QString name = knownhosts::hostName(connection_.host, connection_.port);
            const QRegularExpressionMatch match =
                QRegularExpression(QStringLiteral("Host key for\\s+(\\S+)\\s+has\\s+changed"))
                    .match(output);
            if (match.hasMatch())
                name = match.captured(1);
            const QRegularExpressionMatch key =
                QRegularExpression(QStringLiteral("sent by the remote host is\\s+(SHA256:[A-Za-z0-9+/=]+)"))
                    .match(output);
            emit hostKeyChanged(this, name, key.hasMatch() ? key.captured(1) : QString());
        }
        recentOutput_.clear();
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
    // "auto" follows the app's light/dark theme (re-applied when it changes).
    QString scheme = profile.colorScheme;
    if (scheme.isEmpty() || scheme == QLatin1String("auto"))
        scheme = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Light
                     ? QStringLiteral("BlackOnWhite")
                     : QStringLiteral("DarkPastels");
    terminal_->setColorScheme(scheme);
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

void SessionView::showEndBar(bool exitKnown, int exitCode)
{
    QStringList parts;
    parts << QStringLiteral("<b>%1</b>").arg(endReason_.text.toHtmlEscaped());
    if (!endReason_.detail.isEmpty() && endReason_.detail != endReason_.text)
        parts << QStringLiteral("<span style=\"opacity:0.8\">%1</span>")
                     .arg(endReason_.detail.left(200).toHtmlEscaped());
    QStringList facts;
    if (exitKnown)
        facts << tr("codice di uscita %1").arg(exitCode);
    if (uptime_.isValid()) {
        const qint64 seconds = uptime_.elapsed() / 1000;
        if (seconds >= 3600)
            facts << tr("durata %1 h %2 min").arg(seconds / 3600).arg((seconds % 3600) / 60);
        else if (seconds >= 60)
            facts << tr("durata %1 min").arg(seconds / 60);
        else
            facts << tr("durata %1 s").arg(seconds);
    }
    if (!facts.isEmpty())
        parts << facts.join(QStringLiteral(" · "));
    endLabel_->setText(parts.join(QStringLiteral(" — ")));
    endIcon_->setPixmap(style()
                            ->standardIcon(endReason_.unexpected ? QStyle::SP_MessageBoxWarning
                                                                 : QStyle::SP_MessageBoxInformation)
                            .pixmap(20, 20));
    endBar_->setStyleSheet(endReason_.unexpected
                               ? QStringLiteral("#sessionEndBar { background: rgba(248,81,73,0.16); }")
                               : QStringLiteral("#sessionEndBar { background: rgba(127,127,127,0.14); }"));
    endBar_->show();
}

void SessionView::setActive(bool active)
{
    active_ = active;
    if (active)
        setAttention(Attention::None);
}

void SessionView::setAttention(Attention attention)
{
    // A bell outranks plain activity until the session is looked at.
    if (attention == attention_ || (attention == Attention::Activity && attention_ == Attention::Bell))
        return;
    attention_ = attention;
    emit attentionChanged(this);
}

bool SessionView::copyOnSelect()
{
    return QSettings().value(QStringLiteral("terminal/copyOnSelect"), false).toBool();
}

void SessionView::setNotifyOnSilence(bool enabled)
{
    notifyOnSilence_ = enabled;
    sawOutput_ = false;
    terminal_->setSilenceTimeout(10);
    terminal_->setMonitorSilence(enabled);
}

void SessionView::start()
{
    if (started_)
        return;
    started_ = true;
    uptime_.start();
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

void SessionView::copySelection()
{
    if (terminal_ && !terminal_->selectedText().isEmpty())
        terminal_->copyClipboard();
}

void SessionView::pasteClipboard()
{
    if (!terminal_)
        return;
    const QString text = QGuiApplication::clipboard()->text();
    if (text.isEmpty())
        return;
    terminal_->pasteClipboard();
    // Broadcast the clipboard text to the other sessions, not the shortcut.
    emit pasteRequested(this, text);
}

void SessionView::showContextMenu(const QPoint &pos)
{
    QMenu menu(this);

    QAction *copy = menu.addAction(
        QIcon::fromTheme(QStringLiteral("edit-copy")), tr("Copia"), this, &SessionView::copySelection);
    copy->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+C")));
    copy->setEnabled(!terminal_->selectedText().isEmpty());

    QAction *paste = menu.addAction(
        QIcon::fromTheme(QStringLiteral("edit-paste")), tr("Incolla"), this, &SessionView::pasteClipboard);
    paste->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+V")));
    paste->setEnabled(!finished_ && !QGuiApplication::clipboard()->text().isEmpty());

    // Link actions ("Apri link", "Copia indirizzo link") for the text under
    // the pointer; positions are in the display's coordinates.
    QWidget *display = terminal_->focusProxy();
    const QPoint displayPos = display ? display->mapFrom(terminal_, pos) : pos;
    const QList<QAction *> linkActions = terminal_->filterActions(displayPos);
    if (!linkActions.isEmpty()) {
        menu.addSeparator();
        menu.addActions(linkActions);
    }

    menu.addSeparator();
    menu.addAction(QIcon::fromTheme(QStringLiteral("edit-clear")), tr("Pulisci terminale"),
                   this, &SessionView::clearTerminal);

    // A selected host or IP ("10.0.0.5", "web01.example.com:2222",
    // "deploy@db01") can be opened directly.
    const QString selection = terminal_->selectedText().trimmed();
    if (!selection.isEmpty() && selection.size() < 256 && !selection.contains(QLatin1Char('\n'))
        && parseQuickTarget(selection, nullptr, nullptr, nullptr)) {
        menu.addAction(QIcon::fromTheme(QStringLiteral("utilities-terminal")),
                       tr("Connessione rapida a %1").arg(selection), this,
                       [this, selection] { emit quickConnectRequested(selection); });
    }

    menu.addSeparator();
    QAction *search = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-find")),
                                     tr("Cerca nel terminale…"), this,
                                     [this] { terminal_->toggleShowSearchBar(); });
    search->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")));
    QAction *copySelect = menu.addAction(tr("Copia alla selezione"));
    copySelect->setToolTip(tr("Copia automaticamente negli appunti il testo selezionato col mouse"));
    copySelect->setCheckable(true);
    copySelect->setChecked(copyOnSelect());
    connect(copySelect, &QAction::toggled, this,
            [](bool on) { QSettings().setValue(QStringLiteral("terminal/copyOnSelect"), on); });

    QAction *notify = menu.addAction(tr("Avvisami quando l'output si ferma"));
    notify->setToolTip(tr("Notifica quando il terminale resta senza output per 10 secondi, "
                          "ad esempio alla fine di un comando lungo"));
    notify->setCheckable(true);
    notify->setChecked(notifyOnSilence_);
    notify->setEnabled(!finished_);
    connect(notify, &QAction::toggled, this, [this](bool on) { setNotifyOnSilence(on); });
    if (finished_) {
        menu.addAction(QIcon::fromTheme(QStringLiteral("view-refresh"),
                                        style()->standardIcon(QStyle::SP_BrowserReload)),
                       tr("Riconnetti"), this, [this] { emit reconnectRequested(this); });
    }

    menu.exec(terminal_->mapToGlobal(pos));
    terminal_->setFocus();
}

void SessionView::clearTerminal()
{
#ifdef _WIN32
    // QTermWidget::clear() resets the local screen and history, then nudges the
    // window size to make the program redraw. Under ConPTY every resize makes
    // the pseudo console repaint its own copy of the screen, which brings the
    // old content straight back. So first let the program clear the screen
    // (Ctrl+L), which empties ConPTY's buffer too, and only once its output
    // has settled clear the local history: the repaint then shows the cleared
    // screen.
    if (!started_ || finished_) {
        terminal_->clear();
        return;
    }
    if (clearPending_)
        return;
    clearPending_ = true;

    auto *quiet = new QTimer(this);
    quiet->setSingleShot(true);
    auto *deadline = new QTimer(this);
    deadline->setSingleShot(true);

    auto finish = [this, quiet, deadline] {
        if (!clearPending_)
            return;
        quiet->stop();
        deadline->stop();
        quiet->deleteLater();
        deadline->deleteLater();
        clearPending_ = false;
        terminal_->clear();
    };
    connect(quiet, &QTimer::timeout, this, finish);
    connect(deadline, &QTimer::timeout, this, finish);
    // Restart the quiet period on every chunk of output; disconnected
    // automatically when the timer is deleted.
    connect(terminal_, &QTermWidget::receivedData, quiet, [this, quiet] {
        if (clearPending_)
            quiet->start(200);
    });

    terminal_->sendText(QString(QChar(0x0c)));
    quiet->start(500); // first wait covers the round trip to the remote host
    deadline->start(2000);
#else
    terminal_->clear();
#endif
}

bool SessionView::eventFilter(QObject *watched, QEvent *event)
{
    // Copy on select: when the mouse button is released over a selection.
    if (event->type() == QEvent::MouseButtonRelease
        && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton && copyOnSelect()
        && !terminal_->selectedText().isEmpty())
        terminal_->copyClipboard();

    if (event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        const Qt::KeyboardModifiers mods =
            key->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier
                                | Qt::MetaModifier);

        // Clipboard shortcuts are handled here and swallowed, so they reach
        // neither the remote shell nor the broadcast. Ctrl+C stays SIGINT.
        const bool copyKey =
            (mods == (Qt::ControlModifier | Qt::ShiftModifier) && key->key() == Qt::Key_C)
            || (mods == Qt::ControlModifier && key->key() == Qt::Key_Insert);
        const bool pasteKey =
            (mods == (Qt::ControlModifier | Qt::ShiftModifier) && key->key() == Qt::Key_V)
            || (mods == Qt::ShiftModifier && key->key() == Qt::Key_Insert);
        if (copyKey) {
            copySelection();
            return true;
        }
        if (mods == (Qt::ControlModifier | Qt::ShiftModifier) && key->key() == Qt::Key_F) {
            terminal_->toggleShowSearchBar();
            return true;
        }
        if (pasteKey) {
            if (!finished_)
                pasteClipboard();
            return true;
        }

        // Enter at a shell prompt: record the command line before the
        // terminal processes the key. Full-screen programs (vim, less...)
        // and lines that do not look like a prompt are ignored.
#ifdef SSHMANAGER_QTW_CURSOR_LINE
        const bool enter = (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
                           && (mods & ~Qt::KeypadModifier) == Qt::NoModifier;
        QWidget *display = terminal_->focusProxy();
        if (enter && started_ && !finished_ && !connection_.local
            && (watched == display || !display) && !terminal_->isAlternateScreen()) {
            const QString command = snippets::commandFromPromptLine(terminal_->cursorLineText());
            if (!command.isEmpty())
                emit commandEntered(this, command);
        }
#endif

        const bool ctrlShift =
            key->modifiers().testFlag(Qt::ControlModifier)
            && key->modifiers().testFlag(Qt::ShiftModifier);
        if (!ctrlShift && !key->modifiers().testFlag(Qt::MetaModifier))
            emit keyPressed(this, key);
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
    terminating_ = true; // closing the window is not a crash
    const int pid = terminal_->getShellPID();
#ifdef _WIN32
    // No SIGHUP on Windows: end ssh.exe right away, so that the temporary key
    // and askpass files can be deleted. The pseudo console (and any jump host
    // process attached to it) is closed when the QTermWidget is destroyed.
    if (pid > 0) {
        if (HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, DWORD(pid))) {
            TerminateProcess(process, 1);
            WaitForSingleObject(process, 2000);
            CloseHandle(process);
        }
    }
#else
    if (pid > 0)
        ::kill(pid, SIGHUP);
#endif
}

void SessionView::cleanupFiles()
{
    for (const QString &path : plan_.temporaryFiles)
        QFile::remove(path);
    plan_.temporaryFiles.clear();
}
