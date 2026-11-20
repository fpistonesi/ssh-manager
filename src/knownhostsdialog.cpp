#include "knownhostsdialog.h"

#include "knownhosts.h"
#include "paths.h"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QStyle>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

enum Column { HostColumn, TypeColumn, FingerprintColumn, AddedColumn, UsedByColumn, ColumnCount };
constexpr int LineRole = Qt::UserRole;
constexpr int FingerprintRole = Qt::UserRole + 1;

QString shortKeyType(const QString &type)
{
    if (type == QLatin1String("ssh-ed25519"))
        return QStringLiteral("ED25519");
    if (type.startsWith(QLatin1String("ecdsa-sha2-")))
        return QStringLiteral("ECDSA");
    if (type == QLatin1String("ssh-rsa"))
        return QStringLiteral("RSA");
    if (type == QLatin1String("ssh-dss"))
        return QStringLiteral("DSA");
    return type;
}

} // namespace

bool isHostKeyPrompt(const QString &prompt)
{
    return prompt.contains(QLatin1String("continue connecting"), Qt::CaseInsensitive)
           || prompt.contains(QLatin1String("authenticity of host"), Qt::CaseInsensitive);
}

bool confirmHostKey(const QString &prompt)
{
    // "The authenticity of host 'name (ip)' can't be established.
    //  ED25519 key fingerprint is SHA256:xxxx.
    //  This key is not known by any other names.
    //  Are you sure you want to continue connecting (yes/no/[fingerprint])?"
    QString host;
    QString keyType;
    QString fingerprint;
    const QRegularExpressionMatch hostMatch =
        QRegularExpression(QStringLiteral("host '([^']+)'")).match(prompt);
    if (hostMatch.hasMatch())
        host = hostMatch.captured(1);
    const QRegularExpressionMatch keyMatch =
        QRegularExpression(QStringLiteral("(\\S+) key fingerprint is (SHA256:[A-Za-z0-9+/=]+)"))
            .match(prompt);
    if (keyMatch.hasMatch()) {
        keyType = keyMatch.captured(1);
        fingerprint = keyMatch.captured(2);
    }

    QDialog dialog;
    dialog.setWindowTitle(QObject::tr("Verifica chiave host"));
    dialog.setWindowFlag(Qt::WindowStaysOnTopHint, true);
    dialog.setMinimumWidth(520);
    auto *layout = new QVBoxLayout(&dialog);

    auto *header = new QHBoxLayout();
    auto *icon = new QLabel(&dialog);
    icon->setPixmap(dialog.style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(40, 40));
    icon->setAlignment(Qt::AlignTop);
    header->addWidget(icon);

    auto *intro = new QLabel(&dialog);
    intro->setWordWrap(true);
    intro->setTextFormat(Qt::RichText);
    intro->setText(
        QObject::tr("<b>Prima connessione a %1</b><br>"
                    "Il server si identifica con una chiave che non è ancora tra quelle "
                    "conosciute. Prima di accettarla, confronta il fingerprint con quello "
                    "fornito dall'amministratore del server.")
            .arg((host.isEmpty() ? QObject::tr("un host sconosciuto") : host).toHtmlEscaped()));
    header->addWidget(intro, 1);
    layout->addLayout(header);

    if (!fingerprint.isEmpty()) {
        auto *keyBox = new QLabel(&dialog);
        keyBox->setTextInteractionFlags(Qt::TextSelectableByMouse);
        keyBox->setTextFormat(Qt::RichText);
        QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        keyBox->setText(QStringLiteral("%1<br><span style=\"font-family:'%2'; font-size:large\">%3</span>")
                            .arg(QObject::tr("Chiave %1").arg(keyType.toHtmlEscaped()),
                                 mono.family(), fingerprint.toHtmlEscaped()));
        keyBox->setContentsMargins(8, 8, 8, 8);
        keyBox->setFrameShape(QFrame::StyledPanel);
        layout->addWidget(keyBox);
    }

    // The rest of ssh's message, e.g. warnings about other names or IPs.
    QStringList extra;
    for (const QString &line : prompt.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.contains(QLatin1String("authenticity of host"))
            || trimmed.contains(QLatin1String("key fingerprint is"))
            || trimmed.contains(QLatin1String("continue connecting")))
            continue;
        extra << trimmed;
    }
    if (!extra.isEmpty()) {
        auto *details = new QLabel(extra.join(QLatin1Char('\n')), &dialog);
        details->setWordWrap(true);
        details->setEnabled(false);
        details->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(details);
    }

    auto *buttons = new QDialogButtonBox(&dialog);
    QPushButton *accept =
        buttons->addButton(QObject::tr("Accetta e connetti"), QDialogButtonBox::AcceptRole);
    QPushButton *cancel = buttons->addButton(QDialogButtonBox::Cancel);
    if (!fingerprint.isEmpty()) {
        QPushButton *copy =
            buttons->addButton(QObject::tr("Copia fingerprint"), QDialogButtonBox::ActionRole);
        QObject::connect(copy, &QPushButton::clicked, &dialog,
                         [fingerprint] { QApplication::clipboard()->setText(fingerprint); });
    }
    // Trusting a key must be a deliberate click, not a reflex Enter.
    cancel->setDefault(true);
    cancel->setFocus();
    accept->setAutoDefault(false);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    dialog.show();
    dialog.raise();
    dialog.activateWindow();
    const bool accepted = dialog.exec() == QDialog::Accepted;
    if (accepted) {
        // The host as ssh will store it ("name", or "[name]:port").
        QString name = host;
        const int paren = name.indexOf(QLatin1String(" ("));
        if (paren > 0)
            name = name.left(paren);
        knownhosts::recordFirstSeen(name, fingerprint);
    }
    return accepted;
}

KnownHostsDialog::KnownHostsDialog(const QVector<QPair<QString, QString>> &connections,
                                   const QString &filter, QWidget *parent)
    : QDialog(parent), connections_(connections)
{
    setWindowTitle(tr("Chiavi host conosciute"));
    resize(900, 480);

    auto *layout = new QVBoxLayout(this);

    pathLabel_ = new QLabel(this);
    pathLabel_->setWordWrap(true);
    pathLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pathLabel_->setText(
        tr("Chiavi accettate da SSH Manager, in %1. Rimuovere una chiave fa chiedere di nuovo "
           "la verifica alla prossima connessione.")
            .arg(QDir::toNativeSeparators(paths::knownHostsFile())));
    layout->addWidget(pathLabel_);

    filter_ = new QLineEdit(this);
    filter_->setPlaceholderText(tr("Filtra per host, connessione o fingerprint…"));
    filter_->setClearButtonEnabled(true);
    filter_->setText(filter);
    layout->addWidget(filter_);

    list_ = new QTreeWidget(this);
    list_->setColumnCount(ColumnCount);
    list_->setHeaderLabels({tr("Host"), tr("Tipo"), tr("Fingerprint (SHA256)"),
                            tr("Prima connessione"), tr("Connessioni")});
    list_->setRootIsDecorated(false);
    list_->setAlternatingRowColors(true);
    list_->setUniformRowHeights(true);
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->setSortingEnabled(true);
    list_->sortByColumn(HostColumn, Qt::AscendingOrder);
    list_->header()->setSectionResizeMode(QHeaderView::Interactive);
    list_->header()->setStretchLastSection(true);
    list_->header()->resizeSection(HostColumn, 220);
    list_->header()->resizeSection(TypeColumn, 70);
    list_->header()->resizeSection(FingerprintColumn, 340);
    list_->header()->resizeSection(AddedColumn, 120);
    layout->addWidget(list_, 1);

    auto *buttons = new QHBoxLayout();
    removeButton_ = new QPushButton(tr("Rimuovi"), this);
    copyButton_ = new QPushButton(tr("Copia fingerprint"), this);
    auto *importButton = new QPushButton(tr("Importa da OpenSSH…"), this);
    importButton->setToolTip(
        tr("Aggiunge le chiavi di %1 non ancora presenti")
            .arg(QDir::toNativeSeparators(QDir::homePath() + QStringLiteral("/.ssh/known_hosts"))));
    importButton->setEnabled(QFileInfo::exists(QDir::homePath() + QStringLiteral("/.ssh/known_hosts")));
    auto *close = new QPushButton(tr("Chiudi"), this);
    buttons->addWidget(removeButton_);
    buttons->addWidget(copyButton_);
    buttons->addWidget(importButton);
    buttons->addStretch(1);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(filter_, &QLineEdit::textChanged, this, &KnownHostsDialog::applyFilter);
    connect(list_, &QTreeWidget::itemSelectionChanged, this, &KnownHostsDialog::updateButtons);
    connect(removeButton_, &QPushButton::clicked, this, &KnownHostsDialog::removeSelected);
    connect(copyButton_, &QPushButton::clicked, this, &KnownHostsDialog::copyFingerprint);
    connect(importButton, &QPushButton::clicked, this, &KnownHostsDialog::importOpenSsh);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);

    reload();
}

void KnownHostsDialog::reload()
{
    QString error;
    const QVector<knownhosts::Entry> entries = knownhosts::read(paths::knownHostsFile(), &error);
    if (!error.isEmpty())
        QMessageBox::warning(this, windowTitle(), tr("Impossibile leggere il file: %1").arg(error));
    const QHash<QString, QDateTime> dates = knownhosts::firstSeen();
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    list_->setSortingEnabled(false);
    list_->clear();
    for (const knownhosts::Entry &entry : entries) {
        auto *item = new QTreeWidgetItem(list_);
        QString hosts = entry.hashed ? tr("(nome cifrato)") : entry.hosts.join(QStringLiteral(", "));
        if (!entry.marker.isEmpty())
            hosts = entry.marker + QLatin1Char(' ') + hosts;
        item->setText(HostColumn, hosts);
        item->setToolTip(HostColumn, entry.hashed
                                         ? tr("Voce scritta con HashKnownHosts: il nome dell'host "
                                              "non è leggibile")
                                         : entry.hosts.join(QLatin1Char('\n')));
        item->setText(TypeColumn, shortKeyType(entry.keyType));
        item->setToolTip(TypeColumn, entry.keyType);
        item->setText(FingerprintColumn, entry.fingerprint);
        item->setFont(FingerprintColumn, mono);
        const QDateTime added = dates.value(entry.fingerprint);
        item->setText(AddedColumn, added.isValid()
                                       ? QLocale().toString(added, QLocale::ShortFormat)
                                       : QStringLiteral("—"));
        if (!added.isValid())
            item->setToolTip(AddedColumn, tr("Chiave accettata prima che SSH Manager registrasse "
                                             "le date, o importata"));

        QStringList usedBy;
        for (const auto &connection : connections_) {
            if (knownhosts::matches(entry, connection.second))
                usedBy << connection.first;
        }
        usedBy.removeDuplicates();
        item->setText(UsedByColumn, usedBy.join(QStringLiteral(", ")));
        item->setData(0, LineRole, entry.line);
        item->setData(0, FingerprintRole, entry.fingerprint);
    }
    list_->setSortingEnabled(true);
    applyFilter();
    updateButtons();
}

void KnownHostsDialog::applyFilter()
{
    const QString needle = filter_->text().trimmed();
    for (int i = 0; i < list_->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = list_->topLevelItem(i);
        bool visible = needle.isEmpty();
        for (int column = 0; !visible && column < ColumnCount; ++column)
            visible = item->text(column).contains(needle, Qt::CaseInsensitive)
                      || item->toolTip(column).contains(needle, Qt::CaseInsensitive);
        item->setHidden(!visible);
    }
}

void KnownHostsDialog::updateButtons()
{
    const int selected = int(list_->selectedItems().size());
    removeButton_->setEnabled(selected > 0);
    copyButton_->setEnabled(selected == 1);
}

void KnownHostsDialog::removeSelected()
{
    const QList<QTreeWidgetItem *> items = list_->selectedItems();
    if (items.isEmpty())
        return;
    const QString what = items.size() == 1
                             ? tr("la chiave di %1").arg(items.first()->text(HostColumn))
                             : tr("%1 chiavi").arg(items.size());
    if (QMessageBox::question(
            this, tr("Rimuovi chiavi"),
            tr("Rimuovere %1? Alla prossima connessione verrà chiesto di verificare di nuovo "
               "la chiave dell'host.")
                .arg(what))
        != QMessageBox::Yes)
        return;

    QSet<int> lines;
    QStringList fingerprints;
    for (QTreeWidgetItem *item : items) {
        lines.insert(item->data(0, LineRole).toInt());
        fingerprints << item->data(0, FingerprintRole).toString();
    }
    QString error;
    if (!knownhosts::removeLines(paths::knownHostsFile(), lines, &error)) {
        QMessageBox::warning(this, tr("Rimuovi chiavi"),
                             tr("Impossibile aggiornare il file: %1").arg(error));
    } else {
        knownhosts::forgetFirstSeen(fingerprints);
    }
    reload();
}

void KnownHostsDialog::copyFingerprint()
{
    const QList<QTreeWidgetItem *> items = list_->selectedItems();
    if (items.size() == 1)
        QApplication::clipboard()->setText(items.first()->text(FingerprintColumn));
}

void KnownHostsDialog::importOpenSsh()
{
    const QString source = QDir::homePath() + QStringLiteral("/.ssh/known_hosts");
    QString error;
    const int added = knownhosts::import(source, paths::knownHostsFile(), &error);
    if (added < 0) {
        QMessageBox::warning(this, tr("Importa da OpenSSH"),
                             tr("Importazione non riuscita: %1").arg(error));
        return;
    }
    QMessageBox::information(this, tr("Importa da OpenSSH"),
                             added == 0 ? tr("Nessuna chiave nuova da importare.")
                                        : tr("Chiavi importate: %1.").arg(added));
    reload();
}
