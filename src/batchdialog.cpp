#include "batchdialog.h"

#include "apptheme.h"
#include "snippetdialog.h"
#include "snippets.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStyle>
#include <QTextStream>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

enum ResultColumn { HostColumn, StateColumn, CodeColumn, TimeColumn };

QString seconds(qint64 ms)
{
    return QStringLiteral("%1 s").arg(QString::number(ms / 1000.0, 'f', ms < 10000 ? 1 : 0));
}

} // namespace

BatchDialog::BatchDialog(const QVector<Host> &hosts, const QStringList &preselected,
                         const QVector<Snippet> &snippets, PlanFactory planFactory, QWidget *parent)
    : QDialog(parent), hosts_(hosts), snippets_(snippets), planFactory_(std::move(planFactory))
{
    setWindowTitle(tr("Esecuzione su più host"));
    setWindowFlag(Qt::WindowMaximizeButtonHint, true);
    resize(1100, 760);

    QSettings settings;
    maxParallel_ = qBound(1, settings.value(QStringLiteral("batch/parallel"), 4).toInt(), 16);
    timeoutMs_ = qBound(5, settings.value(QStringLiteral("batch/timeout"), 60).toInt(), 3600) * 1000;

    auto *layout = new QVBoxLayout(this);
    auto *vertical = new QSplitter(Qt::Vertical, this);
    layout->addWidget(vertical, 1);

    // --- Hosts and command ------------------------------------------------
    auto *top = new QSplitter(Qt::Horizontal, vertical);

    auto *hostBox = new QGroupBox(tr("Host"), top);
    auto *hostLayout = new QVBoxLayout(hostBox);
    hostList_ = new QTreeWidget(hostBox);
    hostList_->setHeaderLabels({tr("Connessione"), tr("Destinazione")});
    hostList_->setRootIsDecorated(false);
    hostList_->setUniformRowHeights(true);
    hostList_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (const Host &host : hosts_) {
        auto *item = new QTreeWidgetItem(hostList_);
        item->setText(0, host.name);
        item->setText(1, host.connection.target());
        item->setData(0, Qt::UserRole, host.id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, preselected.contains(host.id) ? Qt::Checked : Qt::Unchecked);
    }
    auto *hostButtons = new QHBoxLayout;
    auto *all = new QPushButton(tr("Tutti"), hostBox);
    auto *none = new QPushButton(tr("Nessuno"), hostBox);
    hostButtons->addWidget(all);
    hostButtons->addWidget(none);
    hostButtons->addStretch();
    hostLayout->addWidget(hostList_, 1);
    hostLayout->addLayout(hostButtons);
    connect(all, &QPushButton::clicked, this, [this] {
        for (int i = 0; i < hostList_->topLevelItemCount(); ++i)
            hostList_->topLevelItem(i)->setCheckState(0, Qt::Checked);
    });
    connect(none, &QPushButton::clicked, this, [this] {
        for (int i = 0; i < hostList_->topLevelItemCount(); ++i)
            hostList_->topLevelItem(i)->setCheckState(0, Qt::Unchecked);
    });

    auto *commandBox = new QGroupBox(tr("Comando"), top);
    auto *commandLayout = new QVBoxLayout(commandBox);
    snippetCombo_ = new QComboBox(commandBox);
    snippetCombo_->addItem(tr("Inserisci uno snippet…"));
    for (const Snippet &snippet : snippets_)
        snippetCombo_->addItem(snippet.name, snippet.command);
    snippetCombo_->setEnabled(!snippets_.isEmpty());
    command_ = new QPlainTextEdit(commandBox);
    command_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    command_->setPlaceholderText(tr("es. uptime && df -h /"));
    command_->setTabChangesFocus(true);
    auto *hint = new QLabel(tr("Eseguito dalla shell dell'utente su ogni host, senza terminale. "
                               "${host}, ${user}, ${port} e ${name} valgono per ciascun host; le "
                               "altre ${variabili} vengono chieste una volta."),
                            commandBox);
    hint->setWordWrap(true);
    hint->setEnabled(false);

    auto *options = new QHBoxLayout;
    options->addWidget(new QLabel(tr("In parallelo:"), commandBox));
    parallel_ = new QSpinBox(commandBox);
    parallel_->setRange(1, 16);
    parallel_->setValue(maxParallel_);
    options->addWidget(parallel_);
    options->addSpacing(12);
    options->addWidget(new QLabel(tr("Timeout:"), commandBox));
    timeout_ = new QSpinBox(commandBox);
    timeout_->setRange(5, 3600);
    timeout_->setSuffix(tr(" s"));
    timeout_->setValue(timeoutMs_ / 1000);
    options->addWidget(timeout_);
    options->addStretch();
    runButton_ = new QPushButton(style()->standardIcon(QStyle::SP_MediaPlay), tr("Esegui"), commandBox);
    runButton_->setDefault(true);
    stopButton_ = new QPushButton(style()->standardIcon(QStyle::SP_MediaStop), tr("Interrompi"), commandBox);
    stopButton_->setEnabled(false);
    options->addWidget(runButton_);
    options->addWidget(stopButton_);

    commandLayout->addWidget(snippetCombo_);
    commandLayout->addWidget(command_, 1);
    commandLayout->addWidget(hint);
    commandLayout->addLayout(options);
    top->setStretchFactor(0, 2);
    top->setStretchFactor(1, 3);

    // --- Results ------------------------------------------------------------
    auto *resultBox = new QGroupBox(tr("Risultati"), vertical);
    auto *resultLayout = new QVBoxLayout(resultBox);
    auto *resultSplit = new QSplitter(Qt::Horizontal, resultBox);
    results_ = new QTreeWidget(resultSplit);
    results_->setHeaderLabels({tr("Host"), tr("Stato"), tr("Codice"), tr("Durata")});
    results_->setRootIsDecorated(false);
    results_->setUniformRowHeights(true);
    results_->header()->setSectionResizeMode(HostColumn, QHeaderView::Stretch);
    results_->header()->setStretchLastSection(false);
    results_->header()->resizeSection(StateColumn, 190);
    results_->header()->resizeSection(CodeColumn, 60);
    results_->header()->resizeSection(TimeColumn, 70);
    output_ = new QPlainTextEdit(resultSplit);
    output_->setReadOnly(true);
    output_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    output_->setPlaceholderText(tr("Seleziona un host per vederne l'output."));
    output_->setLineWrapMode(QPlainTextEdit::NoWrap);
    resultSplit->setStretchFactor(0, 2);
    resultSplit->setStretchFactor(1, 3);
    resultLayout->addWidget(resultSplit, 1);

    auto *resultBar = new QHBoxLayout;
    summary_ = new QLabel(resultBox);
    rerunButton_ = new QPushButton(tr("Riesegui sui non riusciti"), resultBox);
    rerunButton_->setEnabled(false);
    auto *copy = new QPushButton(tr("Copia output"), resultBox);
    saveButton_ = new QPushButton(tr("Salva risultati…"), resultBox);
    saveButton_->setEnabled(false);
    auto *close = new QPushButton(tr("Chiudi"), resultBox);
    resultBar->addWidget(summary_, 1);
    resultBar->addWidget(rerunButton_);
    resultBar->addWidget(copy);
    resultBar->addWidget(saveButton_);
    resultBar->addWidget(close);
    resultLayout->addLayout(resultBar);
    vertical->setStretchFactor(0, 2);
    vertical->setStretchFactor(1, 3);

    connect(snippetCombo_, &QComboBox::activated, this, [this](int index) {
        if (index > 0)
            command_->setPlainText(snippetCombo_->itemData(index).toString());
        snippetCombo_->setCurrentIndex(0);
    });
    connect(runButton_, &QPushButton::clicked, this, [this] { start(checkedIds()); });
    connect(stopButton_, &QPushButton::clicked, this, &BatchDialog::stopAll);
    connect(rerunButton_, &QPushButton::clicked, this, [this] {
        QStringList failed;
        for (const Run &run : runs_) {
            if (run.done && !run.result.ok())
                failed << run.id;
        }
        command_->setPlainText(lastCommand_);
        start(failed);
    });
    connect(results_, &QTreeWidget::currentItemChanged, this, [this] { showOutput(); });
    connect(copy, &QPushButton::clicked, this,
            [this] { QApplication::clipboard()->setText(output_->toPlainText()); });
    connect(saveButton_, &QPushButton::clicked, this, &BatchDialog::save);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(parallel_, &QSpinBox::valueChanged, this, [this](int value) {
        maxParallel_ = value;
        QSettings().setValue(QStringLiteral("batch/parallel"), value);
        launchNext();
    });
    connect(timeout_, &QSpinBox::valueChanged, this, [this](int value) {
        timeoutMs_ = value * 1000;
        QSettings().setValue(QStringLiteral("batch/timeout"), value);
    });

    updateSummary();
    command_->setFocus();
}

BatchDialog::~BatchDialog()
{
    stopAll();
    for (Run &run : runs_) {
        delete run.process;
        run.process = nullptr;
        for (const QString &path : run.temporaryFiles)
            QFile::remove(path);
    }
}

const BatchDialog::Host *BatchDialog::host(const QString &id) const
{
    for (const Host &h : hosts_) {
        if (h.id == id)
            return &h;
    }
    return nullptr;
}

QStringList BatchDialog::checkedIds() const
{
    QStringList ids;
    for (int i = 0; i < hostList_->topLevelItemCount(); ++i) {
        const QTreeWidgetItem *item = hostList_->topLevelItem(i);
        if (item->checkState(0) == Qt::Checked)
            ids << item->data(0, Qt::UserRole).toString();
    }
    return ids;
}

void BatchDialog::start(const QStringList &ids)
{
    for (const Run &run : runs_) {
        if (run.started && !run.done)
            return; // still running
    }
    const QString command = command_->toPlainText().trimmed();
    if (command.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), tr("Scrivi il comando da eseguire."));
        return;
    }
    if (ids.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), tr("Seleziona almeno un host."));
        return;
    }
    if (ids.size() > 1
        && QMessageBox::question(this, windowTitle(),
                                 tr("Eseguire il comando su %1 host?").arg(ids.size()))
               != QMessageBox::Yes)
        return;

    // Custom ${variables}: asked once, the same for every host.
    QString base = command;
    const QStringList variables = snippets::customVariables(base);
    if (!variables.isEmpty()) {
        if (!askSnippetVariables(this, windowTitle(), variables, &variableValues_))
            return;
        base = snippets::applyVariables(base, variableValues_);
    }

    // Previous results go away (and with them their temporary files).
    for (Run &run : runs_) {
        delete run.process;
        for (const QString &path : run.temporaryFiles)
            QFile::remove(path);
    }
    runs_.clear();
    results_->clear();
    output_->clear();
    lastCommand_ = command;
    startedAt_ = QDateTime::currentDateTime();

    for (const QString &id : ids) {
        const Host *h = host(id);
        if (!h)
            continue;
        Run run;
        run.id = id;
        run.expandedCommand = snippets::expandBuiltins(base, h->connection);
        run.row = new QTreeWidgetItem(results_);
        run.row->setText(HostColumn, h->name);
        run.row->setToolTip(HostColumn, h->connection.target());
        run.row->setData(0, Qt::UserRole, id);
        run.row->setText(StateColumn, tr("In attesa"));
        runs_.append(run);
    }
    if (results_->topLevelItemCount() > 0)
        results_->setCurrentItem(results_->topLevelItem(0));
    runButton_->setEnabled(false);
    stopButton_->setEnabled(true);
    rerunButton_->setEnabled(false);
    saveButton_->setEnabled(false);
    launchNext();
    updateSummary();
}

void BatchDialog::launchNext()
{
    int running = 0;
    for (const Run &run : runs_) {
        if (run.started && !run.done)
            ++running;
    }
    for (Run &run : runs_) {
        if (running >= maxParallel_)
            break;
        if (run.started)
            continue;
        run.started = true;

        // May ask for a password: plans are built only when their turn comes.
        const SshPlan plan = planFactory_(run.id);
        run.temporaryFiles = plan.temporaryFiles;
        if (!plan.valid) {
            RemoteRun::Result failed;
            failed.startError = plan.error;
            run.row->setText(StateColumn, tr("In corso"));
            onFinished(run.id, failed);
            continue;
        }
        run.process = new RemoteRun(plan, run.expandedCommand, QByteArray(), timeoutMs_, this);
        const QString id = run.id;
        connect(run.process, &RemoteRun::finished, this,
                [this, id](const RemoteRun::Result &result) { onFinished(id, result); });
        run.row->setText(StateColumn, tr("In corso"));
        run.row->setIcon(StateColumn, style()->standardIcon(QStyle::SP_BrowserReload));
        ++running;
        run.process->start();
    }
    updateSummary();
}

void BatchDialog::onFinished(const QString &id, const RemoteRun::Result &result)
{
    for (Run &run : runs_) {
        if (run.id != id || run.done)
            continue;
        run.done = true;
        run.result = result;
        // Credentials are no longer needed for this host.
        for (const QString &path : run.temporaryFiles)
            QFile::remove(path);
        run.temporaryFiles.clear();

        QString state;
        QStyle::StandardPixmap icon = QStyle::SP_DialogApplyButton;
        QColor color = apptheme::success();
        if (!result.startError.isEmpty()) {
            state = tr("Non avviato");
            icon = QStyle::SP_MessageBoxCritical;
            color = apptheme::danger();
        } else if (result.cancelled) {
            state = tr("Interrotto");
            icon = QStyle::SP_MediaStop;
            color = apptheme::muted();
        } else if (result.timedOut) {
            state = tr("Timeout");
            icon = QStyle::SP_MessageBoxWarning;
            color = apptheme::warning();
        } else if (result.exitCode == 255) {
            state = tr("Errore di connessione");
            icon = QStyle::SP_MessageBoxCritical;
            color = apptheme::danger();
        } else if (result.exitCode != 0) {
            state = tr("Uscito con errore");
            icon = QStyle::SP_MessageBoxWarning;
            color = apptheme::danger();
        } else {
            state = tr("Completato");
        }
        run.row->setText(StateColumn, state);
        run.row->setIcon(StateColumn, style()->standardIcon(icon));
        run.row->setForeground(StateColumn, color);
        run.row->setText(CodeColumn,
                         result.startError.isEmpty() && !result.timedOut && !result.cancelled
                             ? QString::number(result.exitCode)
                             : QStringLiteral("—"));
        run.row->setText(TimeColumn, seconds(result.elapsedMs));
        if (results_->currentItem() == run.row)
            showOutput();
        break;
    }

    bool allDone = true;
    for (const Run &run : runs_)
        allDone = allDone && run.done;
    if (allDone) {
        runButton_->setEnabled(true);
        stopButton_->setEnabled(false);
        saveButton_->setEnabled(!runs_.isEmpty());
        bool anyFailed = false;
        for (const Run &run : runs_)
            anyFailed = anyFailed || !run.result.ok();
        rerunButton_->setEnabled(anyFailed);
    }
    launchNext();
    updateSummary();
}

void BatchDialog::stopAll()
{
    for (Run &run : runs_) {
        if (!run.started) {
            run.started = true;
            RemoteRun::Result cancelled;
            cancelled.cancelled = true;
            onFinished(run.id, cancelled);
        } else if (run.process && !run.done) {
            run.process->cancel();
        }
    }
}

void BatchDialog::showOutput()
{
    const QTreeWidgetItem *item = results_->currentItem();
    if (!item) {
        output_->clear();
        return;
    }
    const QString id = item->data(0, Qt::UserRole).toString();
    for (const Run &run : runs_) {
        if (run.id != id)
            continue;
        if (!run.done) {
            output_->setPlainText(run.started ? tr("In esecuzione…") : tr("In attesa…"));
            return;
        }
        QString text;
        if (!run.result.startError.isEmpty())
            text += run.result.startError + QLatin1Char('\n');
        text += QString::fromUtf8(run.result.out);
        if (!run.result.err.isEmpty()) {
            if (!text.isEmpty() && !text.endsWith(QLatin1Char('\n')))
                text += QLatin1Char('\n');
            text += tr("--- stderr ---\n") + QString::fromUtf8(run.result.err);
        }
        if (run.result.truncated)
            text += tr("\n[output troncato a 1 MB]");
        if (run.result.timedOut)
            text += tr("\n[interrotto dopo il timeout]");
        output_->setPlainText(text.isEmpty() ? tr("(nessun output)") : text);
        return;
    }
}

void BatchDialog::updateSummary()
{
    int ok = 0;
    int failed = 0;
    int running = 0;
    int waiting = 0;
    for (const Run &run : runs_) {
        if (run.done)
            run.result.ok() ? ++ok : ++failed;
        else if (run.started)
            ++running;
        else
            ++waiting;
    }
    summary_->setText(runs_.isEmpty()
                          ? tr("Scegli gli host, scrivi il comando e premi Esegui.")
                          : tr("Completati: %1 · non riusciti: %2 · in corso: %3 · in attesa: %4")
                                .arg(ok)
                                .arg(failed)
                                .arg(running)
                                .arg(waiting));
}

void BatchDialog::save()
{
    QString selectedFilter;
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Salva risultati"),
        QStringLiteral("batch-%1.txt").arg(startedAt_.toString(QStringLiteral("yyyyMMdd-HHmmss"))),
        tr("Testo (*.txt);;JSON (*.json)"), &selectedFilter);
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("Salva risultati"), file.errorString());
        return;
    }

    if (path.endsWith(QLatin1String(".json"), Qt::CaseInsensitive)) {
        QJsonArray hosts;
        for (const Run &run : runs_) {
            const Host *h = host(run.id);
            QJsonObject o;
            o[QStringLiteral("name")] = h ? h->name : run.id;
            o[QStringLiteral("target")] = h ? h->connection.target() : QString();
            o[QStringLiteral("command")] = run.expandedCommand;
            o[QStringLiteral("exitCode")] = run.result.exitCode;
            o[QStringLiteral("timedOut")] = run.result.timedOut;
            o[QStringLiteral("cancelled")] = run.result.cancelled;
            o[QStringLiteral("error")] = run.result.startError;
            o[QStringLiteral("durationMs")] = double(run.result.elapsedMs);
            o[QStringLiteral("stdout")] = QString::fromUtf8(run.result.out);
            o[QStringLiteral("stderr")] = QString::fromUtf8(run.result.err);
            hosts.append(o);
        }
        QJsonObject document;
        document[QStringLiteral("command")] = lastCommand_;
        document[QStringLiteral("startedAt")] = startedAt_.toString(Qt::ISODate);
        document[QStringLiteral("hosts")] = hosts;
        file.write(QJsonDocument(document).toJson(QJsonDocument::Indented));
        return;
    }

    QTextStream stream(&file);
    stream << "# " << lastCommand_ << "\n# " << startedAt_.toString(Qt::ISODate) << "\n\n";
    for (const Run &run : runs_) {
        const Host *h = host(run.id);
        stream << "=== " << (h ? h->name : run.id) << " (" << (h ? h->connection.target() : QString())
               << ") — " << tr("codice %1").arg(run.result.exitCode) << ", "
               << seconds(run.result.elapsedMs) << " ===\n";
        if (!run.result.startError.isEmpty())
            stream << run.result.startError << "\n";
        stream << QString::fromUtf8(run.result.out);
        if (!run.result.err.isEmpty())
            stream << "--- stderr ---\n" << QString::fromUtf8(run.result.err);
        stream << "\n";
    }
}

void BatchDialog::reject()
{
    bool running = false;
    for (const Run &run : runs_)
        running = running || (run.started && !run.done);
    if (running
        && QMessageBox::question(this, windowTitle(),
                                 tr("Ci sono comandi ancora in esecuzione. Interromperli e chiudere?"))
               != QMessageBox::Yes)
        return;
    stopAll();
    QDialog::reject();
}
