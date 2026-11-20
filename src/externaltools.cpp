#include "externaltools.h"

#include "snippets.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace externaltools {

namespace {

const char *modeKey(ExternalTool::Mode mode)
{
    switch (mode) {
    case ExternalTool::Mode::Detached:
        return "detached";
    case ExternalTool::Mode::Url:
        return "url";
    case ExternalTool::Mode::Terminal:
        break;
    }
    return "terminal";
}

ExternalTool::Mode modeFromKey(const QString &key)
{
    if (key == QLatin1String("detached"))
        return ExternalTool::Mode::Detached;
    if (key == QLatin1String("url"))
        return ExternalTool::Mode::Url;
    return ExternalTool::Mode::Terminal;
}

} // namespace

QVector<ExternalTool> defaults()
{
    using Mode = ExternalTool::Mode;
#ifdef Q_OS_WIN
    return {
        {QStringLiteral("Ping"), QStringLiteral("ping -t ${host}"), Mode::Terminal},
        {QStringLiteral("Traceroute"), QStringLiteral("tracert ${host}"), Mode::Terminal},
        {QCoreApplication::translate("ExternalToolsDialog", "Test porta"),
         QStringLiteral("powershell -NoProfile -Command Test-NetConnection ${host} -Port ${port}"),
         Mode::Terminal},
        {QStringLiteral("FTP"), QStringLiteral("ftp ${host}"), Mode::Terminal},
        {QStringLiteral("Browser (HTTPS)"), QStringLiteral("https://${host}/"), Mode::Url},
    };
#else
    return {
        {QStringLiteral("Ping"), QStringLiteral("ping ${host}"), Mode::Terminal},
        {QStringLiteral("Traceroute"), QStringLiteral("traceroute ${host}"), Mode::Terminal},
        {QCoreApplication::translate("ExternalToolsDialog", "Test porta"), QStringLiteral("nc -vz ${host} ${port}"), Mode::Terminal},
        {QStringLiteral("FTP"), QStringLiteral("ftp ${host}"), Mode::Terminal},
        {QStringLiteral("Browser (HTTPS)"), QStringLiteral("https://${host}/"), Mode::Url},
    };
#endif
}

QVector<ExternalTool> load()
{
    QSettings settings;
    if (!settings.contains(QStringLiteral("tools/initialized")))
        return defaults();
    QVector<ExternalTool> tools;
    const int count = settings.beginReadArray(QStringLiteral("tools/list"));
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        ExternalTool tool;
        tool.name = settings.value(QStringLiteral("name")).toString();
        tool.command = settings.value(QStringLiteral("command")).toString();
        tool.mode = modeFromKey(settings.value(QStringLiteral("mode")).toString());
        if (!tool.command.isEmpty())
            tools.append(tool);
    }
    settings.endArray();
    return tools;
}

void save(const QVector<ExternalTool> &tools)
{
    QSettings settings;
    settings.remove(QStringLiteral("tools/list"));
    settings.beginWriteArray(QStringLiteral("tools/list"), int(tools.size()));
    for (int i = 0; i < tools.size(); ++i) {
        settings.setArrayIndex(i);
        settings.setValue(QStringLiteral("name"), tools.at(i).name);
        settings.setValue(QStringLiteral("command"), tools.at(i).command);
        settings.setValue(QStringLiteral("mode"), QString::fromLatin1(modeKey(tools.at(i).mode)));
    }
    settings.endArray();
    settings.setValue(QStringLiteral("tools/initialized"), true);
}

QStringList expandCommand(const QString &command, const Connection &connection,
                          const QHash<QString, QString> &custom)
{
    QStringList tokens = QProcess::splitCommand(command);
    for (QString &token : tokens)
        token = snippets::expandBuiltins(snippets::applyVariables(token, custom), connection);
    if (!tokens.isEmpty()) {
        const QString found = QStandardPaths::findExecutable(tokens.first());
        if (!found.isEmpty())
            tokens.first() = QDir::toNativeSeparators(found);
    }
    return tokens;
}

} // namespace externaltools

ExternalToolsDialog::ExternalToolsDialog(QWidget *parent)
    : QDialog(parent), tools_(externaltools::load())
{
    setWindowTitle(tr("Strumenti esterni"));
    resize(720, 400);

    auto *layout = new QVBoxLayout(this);
    auto *body = new QHBoxLayout();

    // List and its buttons.
    auto *left = new QVBoxLayout();
    list_ = new QListWidget(this);
    left->addWidget(list_, 1);
    auto *listButtons = new QHBoxLayout();
    auto *add = new QPushButton(tr("Aggiungi"), this);
    remove_ = new QPushButton(tr("Rimuovi"), this);
    up_ = new QPushButton(tr("Su"), this);
    down_ = new QPushButton(tr("Giù"), this);
    for (QPushButton *button : {add, remove_, up_, down_})
        listButtons->addWidget(button);
    left->addLayout(listButtons);
    body->addLayout(left, 2);

    // Selected tool.
    auto *right = new QVBoxLayout();
    auto *form = new QFormLayout();
    name_ = new QLineEdit(this);
    name_->setPlaceholderText(tr("es. Ping"));
    form->addRow(tr("Nome"), name_);

    auto *commandRow = new QHBoxLayout();
    command_ = new QLineEdit(this);
    command_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    command_->setPlaceholderText(tr("es. ping -t ${host}"));
    browse_ = new QPushButton(tr("Sfoglia…"), this);
    browse_->setToolTip(tr("Scegli il programma da avviare"));
    commandRow->addWidget(command_, 1);
    commandRow->addWidget(browse_);
    form->addRow(tr("Comando"), commandRow);

    mode_ = new QComboBox(this);
    mode_->addItem(tr("Scheda terminale integrata"), int(ExternalTool::Mode::Terminal));
    mode_->addItem(tr("Programma esterno (finestra propria)"), int(ExternalTool::Mode::Detached));
    mode_->addItem(tr("Apri indirizzo (URL)"), int(ExternalTool::Mode::Url));
    form->addRow(tr("Esecuzione"), mode_);
    right->addLayout(form);

    auto *hint = new QLabel(
        tr("Variabili: ${host}, ${port}, ${user} e ${name} vengono dalla connessione; ogni "
           "altra ${variabile} viene chiesta all'avvio. Gli argomenti con spazi vanno tra "
           "virgolette. Gli strumenti sono salvati nelle impostazioni dell'applicazione, non "
           "nell'archivio: non inserire password."),
        this);
    hint->setWordWrap(true);
    hint->setEnabled(false);
    right->addWidget(hint);
    right->addStretch();
    auto *restore = new QPushButton(tr("Aggiungi strumenti predefiniti"), this);
    restore->setToolTip(tr("Aggiunge in fondo all'elenco gli strumenti di esempio mancanti"));
    right->addWidget(restore, 0, Qt::AlignLeft);
    body->addLayout(right, 3);
    layout->addLayout(body, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &ExternalToolsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(list_, &QListWidget::currentRowChanged, this, [this](int row) {
        storeCurrent();
        showTool(row);
    });
    for (QLineEdit *edit : {name_, command_})
        connect(edit, &QLineEdit::textEdited, this, &ExternalToolsDialog::storeCurrent);
    connect(mode_, &QComboBox::currentIndexChanged, this, [this] {
        storeCurrent();
        browse_->setEnabled(current_ >= 0
                            && mode_->currentData().toInt() != int(ExternalTool::Mode::Url));
    });
    connect(browse_, &QPushButton::clicked, this, &ExternalToolsDialog::browseProgram);
    connect(add, &QPushButton::clicked, this, [this] {
        storeCurrent();
        ExternalTool tool;
        tool.name = tr("Nuovo strumento");
        tools_.append(tool);
        rebuildList(int(tools_.size()) - 1);
        name_->setFocus();
        name_->selectAll();
    });
    connect(remove_, &QPushButton::clicked, this, [this] {
        if (current_ < 0)
            return;
        tools_.removeAt(current_);
        current_ = -1;
        rebuildList(qMin(list_->currentRow(), int(tools_.size()) - 1));
    });
    const auto move = [this](int delta) {
        storeCurrent();
        const int from = current_;
        const int to = from + delta;
        if (from < 0 || to < 0 || to >= tools_.size())
            return;
        tools_.swapItemsAt(from, to);
        current_ = -1;
        rebuildList(to);
    };
    connect(up_, &QPushButton::clicked, this, [move] { move(-1); });
    connect(down_, &QPushButton::clicked, this, [move] { move(1); });
    connect(restore, &QPushButton::clicked, this, [this] {
        storeCurrent();
        int added = 0;
        for (const ExternalTool &tool : externaltools::defaults()) {
            const bool present = std::any_of(tools_.cbegin(), tools_.cend(), [&](const ExternalTool &t) {
                return t.command == tool.command;
            });
            if (!present) {
                tools_.append(tool);
                ++added;
            }
        }
        current_ = -1;
        rebuildList(added ? int(tools_.size()) - 1 : list_->currentRow());
    });

    rebuildList(tools_.isEmpty() ? -1 : 0);
}

void ExternalToolsDialog::rebuildList(int select)
{
    loading_ = true;
    list_->clear();
    for (const ExternalTool &tool : tools_)
        list_->addItem(tool.name.isEmpty() ? tool.command : tool.name);
    current_ = -1;
    loading_ = false;
    list_->setCurrentRow(select);
    if (select < 0)
        showTool(-1);
}

void ExternalToolsDialog::showTool(int row)
{
    if (loading_)
        return;
    loading_ = true;
    current_ = (row >= 0 && row < tools_.size()) ? row : -1;
    const bool valid = current_ >= 0;
    const ExternalTool tool = valid ? tools_.at(current_) : ExternalTool();
    name_->setText(tool.name);
    command_->setText(tool.command);
    mode_->setCurrentIndex(mode_->findData(int(tool.mode)));
    for (QWidget *widget : std::initializer_list<QWidget *>{name_, command_, mode_, remove_})
        widget->setEnabled(valid);
    browse_->setEnabled(valid && tool.mode != ExternalTool::Mode::Url);
    up_->setEnabled(valid && current_ > 0);
    down_->setEnabled(valid && current_ < tools_.size() - 1);
    loading_ = false;
}

void ExternalToolsDialog::storeCurrent()
{
    if (loading_ || current_ < 0 || current_ >= tools_.size())
        return;
    ExternalTool &tool = tools_[current_];
    tool.name = name_->text().trimmed();
    tool.command = command_->text().trimmed();
    tool.mode = ExternalTool::Mode(mode_->currentData().toInt());
    if (QListWidgetItem *item = list_->item(current_))
        item->setText(tool.name.isEmpty() ? tool.command : tool.name);
}

void ExternalToolsDialog::browseProgram()
{
    if (current_ < 0)
        return;
#ifdef Q_OS_WIN
    const QString filter = tr("Programmi (*.exe *.bat *.cmd);;Tutti i file (*)");
#else
    const QString filter;
#endif
    const QString file = QFileDialog::getOpenFileName(this, tr("Scegli il programma"), QString(), filter);
    if (file.isEmpty())
        return;
    const auto quoted = [](const QString &token) {
        return token.contains(QLatin1Char(' ')) ? QLatin1Char('"') + token + QLatin1Char('"')
                                                : token;
    };
    // Keep the arguments already typed after the program.
    QStringList tokens = QProcess::splitCommand(command_->text());
    QStringList arguments;
    for (int i = 1; i < tokens.size(); ++i)
        arguments << quoted(tokens.at(i));
    if (arguments.isEmpty())
        arguments << QStringLiteral("${host}");
    command_->setText(quoted(QDir::toNativeSeparators(file)) + QLatin1Char(' ')
                      + arguments.join(QLatin1Char(' ')));
    storeCurrent();
}

void ExternalToolsDialog::accept()
{
    storeCurrent();
    for (int i = 0; i < tools_.size(); ++i) {
        if (tools_.at(i).command.isEmpty()) {
            list_->setCurrentRow(i);
            QMessageBox::warning(this, windowTitle(), tr("Indica il comando dello strumento."));
            command_->setFocus();
            return;
        }
        if (tools_.at(i).name.isEmpty())
            tools_[i].name = QProcess::splitCommand(tools_.at(i).command).value(0);
    }
    externaltools::save(tools_);
    QDialog::accept();
}
