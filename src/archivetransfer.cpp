#include "archivetransfer.h"

#include "crypto.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

#include <functional>

namespace archivetransfer {

Node subset(const Node &root, const QSet<QString> &hostIds)
{
    Node copy = root;
    copy.children.clear();
    for (const Node &child : root.children) {
        if (child.kind == Node::Kind::Host) {
            if (hostIds.contains(child.id))
                copy.children.append(child);
        } else {
            Node folder = subset(child, hostIds);
            if (!folder.children.isEmpty())
                copy.children.append(folder);
        }
    }
    return copy;
}

int addJumpHosts(const ConnectionStore &store, QSet<QString> *hostIds)
{
    int added = 0;
    QStringList pending(hostIds->cbegin(), hostIds->cend());
    while (!pending.isEmpty()) {
        const Connection *connection = store.findConnection(pending.takeLast());
        if (!connection || connection->jumpId.isEmpty() || hostIds->contains(connection->jumpId))
            continue;
        if (!store.findConnection(connection->jumpId))
            continue;
        hostIds->insert(connection->jumpId);
        pending << connection->jumpId;
        ++added;
    }
    return added;
}

void stripCredentials(Node &node)
{
    if (node.kind == Node::Kind::Host) {
        node.conn.clearSensitive();
        node.conn.keyName.clear(); // the embedded key is gone
    }
    for (Node &child : node.children)
        stripCredentials(child);
}

namespace {

bool containsSelected(const Node &node, const QSet<QString> &hostIds)
{
    if (node.kind == Node::Kind::Host)
        return hostIds.contains(node.id);
    for (const Node &child : node.children) {
        if (containsSelected(child, hostIds))
            return true;
    }
    return false;
}

void mergeFolder(ConnectionStore &store, const QString &destinationId, const Node &source,
                 const QSet<QString> &hostIds, Conflict conflict, QHash<QString, QString> &newIds,
                 QStringList &addedIds, QStringList &createdFolders, MergeResult &result)
{
    for (const Node &child : source.children) {
        if (child.kind == Node::Kind::Folder) {
            if (!containsSelected(child, hostIds))
                continue;
            Node *destination = store.findFolder(destinationId);
            if (!destination)
                return;
            QString folderId;
            for (const Node &existing : std::as_const(destination->children)) {
                if (existing.kind == Node::Kind::Folder
                    && existing.name.compare(child.name, Qt::CaseInsensitive) == 0) {
                    folderId = existing.id;
                    break;
                }
            }
            if (folderId.isEmpty()) {
                Node folder;
                folder.kind = Node::Kind::Folder;
                folder.id = ConnectionStore::newId();
                folder.name = child.name;
                folder.expanded = true;
                destination->children.append(folder);
                folderId = folder.id;
                createdFolders << folderId;
            }
            mergeFolder(store, folderId, child, hostIds, conflict, newIds, addedIds,
                        createdFolders, result);
            continue;
        }

        if (!hostIds.contains(child.id))
            continue;
        Node *existing = store.findNode(child.id);
        if (existing && existing->kind == Node::Kind::Host) {
            if (conflict == Conflict::Skip) {
                ++result.skipped;
                continue;
            }
            if (conflict == Conflict::Update) {
                Connection updated = child.conn;
                // A file without credentials does not wipe the ones we have.
                if (updated.password.isEmpty())
                    updated.password = existing->conn.password;
                if (updated.passphrase.isEmpty())
                    updated.passphrase = existing->conn.passphrase;
                if (updated.privateKey.isEmpty()) {
                    updated.privateKey = existing->conn.privateKey;
                    if (updated.keyName.isEmpty())
                        updated.keyName = existing->conn.keyName;
                }
                existing->conn.clearSensitive();
                existing->conn = updated;
                existing->name = child.name;
                ++result.updated;
                continue;
            }
        }
        Node host = child;
        if (existing) { // copy, or an id clash with a folder
            host.id = ConnectionStore::newId();
            host.conn.id = host.id;
            newIds.insert(child.id, host.id);
            ++result.copied;
        } else {
            ++result.added;
        }
        if (Node *destination = store.findFolder(destinationId)) {
            destination->children.append(host);
            addedIds << host.id;
        }
    }
}

} // namespace

MergeResult merge(ConnectionStore &store, const QString &destinationId, const Node &imported,
                  const QSet<QString> &hostIds, Conflict conflict)
{
    MergeResult result;
    QHash<QString, QString> newIds;
    QStringList addedIds;
    QStringList createdFolders;
    const QString destination =
        store.findFolder(destinationId) ? destinationId : store.root.id;
    mergeFolder(store, destination, imported, hostIds, conflict, newIds, addedIds, createdFolders,
                result);
    // New folders whose hosts were all updated in place or skipped stay
    // empty: drop them, innermost first.
    for (auto it = createdFolders.crbegin(); it != createdFolders.crend(); ++it) {
        const Node *folder = store.findFolder(*it);
        Node *parent = store.parentOf(*it);
        if (!folder || !parent || !folder->children.isEmpty())
            continue;
        const QString id = *it;
        parent->children.erase(std::remove_if(parent->children.begin(), parent->children.end(),
                                              [&](const Node &n) { return n.id == id; }),
                               parent->children.end());
    }
    // Copies keep pointing at the copies of their jump hosts.
    for (const QString &id : std::as_const(addedIds)) {
        if (Connection *connection = store.findConnection(id)) {
            if (newIds.contains(connection->jumpId))
                connection->jumpId = newIds.value(connection->jumpId);
        }
    }
    return result;
}

} // namespace archivetransfer

// --- ConnectionPicker -------------------------------------------------------

ConnectionPicker::ConnectionPicker(QWidget *parent) : QTreeWidget(parent)
{
    setColumnCount(2);
    setHeaderLabels({tr("Nome"), tr("Destinazione")});
    header()->setStretchLastSection(true);
    header()->resizeSection(0, 240);
    setUniformRowHeights(true);
    setAlternatingRowColors(true);
    connect(this, &QTreeWidget::itemChanged, this, [this] { emit selectionChanged(); });
}

void ConnectionPicker::setRoot(const Node &root, const QSet<QString> &checked)
{
    const QSignalBlocker blocker(this);
    clear();
    for (const Node &child : root.children)
        addNode(nullptr, child, checked);
    expandAll();
    emit selectionChanged();
}

void ConnectionPicker::addNode(QTreeWidgetItem *parent, const Node &node, const QSet<QString> &checked)
{
    auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(this);
    item->setText(0, node.name.isEmpty() ? node.conn.displayName() : node.name);
    item->setData(0, Qt::UserRole, node.id);
    item->setData(0, Qt::UserRole + 1, int(node.kind));
    if (node.kind == Node::Kind::Folder) {
        item->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
        for (const Node &child : node.children)
            addNode(item, child, checked);
        if (item->childCount() == 0)
            item->setCheckState(0, Qt::Unchecked);
    } else {
        item->setIcon(0, style()->standardIcon(QStyle::SP_ComputerIcon));
        item->setText(1, node.conn.target());
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, checked.isEmpty() || checked.contains(node.id) ? Qt::Checked
                                                                            : Qt::Unchecked);
    }
}

QSet<QString> ConnectionPicker::checkedHostIds() const
{
    QSet<QString> ids;
    std::function<void(QTreeWidgetItem *)> walk = [&](QTreeWidgetItem *item) {
        if (item->data(0, Qt::UserRole + 1).toInt() == int(Node::Kind::Host)) {
            if (item->checkState(0) == Qt::Checked)
                ids.insert(item->data(0, Qt::UserRole).toString());
            return;
        }
        for (int i = 0; i < item->childCount(); ++i)
            walk(item->child(i));
    };
    for (int i = 0; i < topLevelItemCount(); ++i)
        walk(topLevelItem(i));
    return ids;
}

void ConnectionPicker::setAllChecked(bool checked)
{
    for (int i = 0; i < topLevelItemCount(); ++i)
        topLevelItem(i)->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
}

namespace {

// "Tutte" / "Nessuna" buttons for a picker.
QHBoxLayout *pickerButtons(ConnectionPicker *picker, QWidget *parent)
{
    auto *row = new QHBoxLayout();
    auto *all = new QPushButton(ConnectionPicker::tr("Seleziona tutto"), parent);
    auto *none = new QPushButton(ConnectionPicker::tr("Deseleziona tutto"), parent);
    QObject::connect(all, &QPushButton::clicked, picker, [picker] { picker->setAllChecked(true); });
    QObject::connect(none, &QPushButton::clicked, picker, [picker] { picker->setAllChecked(false); });
    row->addWidget(all);
    row->addWidget(none);
    row->addStretch();
    return row;
}

} // namespace

// --- ExportSelectionDialog --------------------------------------------------

ExportSelectionDialog::ExportSelectionDialog(const ConnectionStore &store,
                                             const QSet<QString> &preselected, QWidget *parent)
    : QDialog(parent), store_(store)
{
    setWindowTitle(tr("Esporta connessioni"));
    resize(640, 520);
    auto *layout = new QVBoxLayout(this);

    auto *intro = new QLabel(tr("Scegli le connessioni da mettere nel file, ad esempio da passare a "
                                "un collega. Il file è cifrato con una password che indicherai "
                                "dopo."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    picker_ = new ConnectionPicker(this);
    picker_->setRoot(store.root, preselected);
    layout->addWidget(picker_, 1);
    layout->addLayout(pickerButtons(picker_, this));

    credentials_ = new QCheckBox(tr("Includi le credenziali (password, passphrase e chiavi "
                                    "incorporate)"),
                                 this);
    credentials_->setToolTip(tr("Senza credenziali chi importa userà le proprie: la password "
                                "viene chiesta alla connessione."));
    layout->addWidget(credentials_);
    snippets_ = new QCheckBox(tr("Includi gli snippet (%n)", "", int(store.snippets.size())), this);
    snippets_->setEnabled(!store.snippets.isEmpty());
    layout->addWidget(snippets_);

    summary_ = new QLabel(this);
    summary_->setWordWrap(true);
    layout->addWidget(summary_);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Esporta…"));
    connect(buttons, &QDialogButtonBox::accepted, this, &ExportSelectionDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(picker_, &ConnectionPicker::selectionChanged, this, &ExportSelectionDialog::updateSummary);
    connect(credentials_, &QCheckBox::toggled, this, &ExportSelectionDialog::updateSummary);
    updateSummary();
}

QSet<QString> ExportSelectionDialog::hostIds() const
{
    QSet<QString> ids = picker_->checkedHostIds();
    archivetransfer::addJumpHosts(store_, &ids);
    return ids;
}

bool ExportSelectionDialog::includeCredentials() const
{
    return credentials_->isChecked();
}

bool ExportSelectionDialog::includeSnippets() const
{
    return snippets_->isChecked();
}

void ExportSelectionDialog::updateSummary()
{
    QSet<QString> ids = picker_->checkedHostIds();
    const int chosen = int(ids.size());
    const int jumps = archivetransfer::addJumpHosts(store_, &ids);
    QString text = tr("%n connessione/i selezionata/e", "", chosen);
    if (jumps > 0)
        text += tr(", più %n jump host necessario/i", "", jumps);
    text += QStringLiteral(". ");
    text += credentials_->isChecked() ? tr("Le credenziali sono incluse: proteggi il file con una "
                                           "password robusta.")
                                      : tr("Senza credenziali.");
    summary_->setText(text);
}

void ExportSelectionDialog::accept()
{
    if (picker_->checkedHostIds().isEmpty()) {
        QMessageBox::warning(this, windowTitle(), tr("Seleziona almeno una connessione."));
        return;
    }
    QDialog::accept();
}

// --- ImportSelectionDialog --------------------------------------------------

ImportSelectionDialog::ImportSelectionDialog(const ConnectionStore &current,
                                             const ConnectionStore &imported, bool allowReplace,
                                             QWidget *parent)
    : QDialog(parent), current_(current), imported_(imported)
{
    setWindowTitle(tr("Importa connessioni"));
    resize(640, 560);
    auto *layout = new QVBoxLayout(this);

    picker_ = new ConnectionPicker(this);
    picker_->setRoot(imported.root);
    layout->addWidget(picker_, 1);
    QHBoxLayout *buttonsRow = pickerButtons(picker_, this);
    layout->addLayout(buttonsRow);

    auto *form = new QFormLayout();
    destination_ = new QComboBox(this);
    destination_->addItem(style()->standardIcon(QStyle::SP_DirIcon), tr("(livello principale)"),
                          current.root.id);
    std::function<void(const Node &, const QString &)> addFolders = [&](const Node &node,
                                                                        const QString &path) {
        for (const Node &child : node.children) {
            if (child.kind != Node::Kind::Folder)
                continue;
            const QString childPath =
                path.isEmpty() ? child.name : path + QStringLiteral(" / ") + child.name;
            destination_->addItem(style()->standardIcon(QStyle::SP_DirIcon), childPath, child.id);
            addFolders(child, childPath);
        }
    };
    addFolders(current.root, QString());
    form->addRow(tr("Aggiungi in"), destination_);

    conflict_ = new QComboBox(this);
    conflict_->addItem(tr("Aggiornale (mantiene le credenziali se il file non le ha)"),
                       int(archivetransfer::Conflict::Update));
    conflict_->addItem(tr("Saltale"), int(archivetransfer::Conflict::Skip));
    conflict_->addItem(tr("Aggiungile come copie"), int(archivetransfer::Conflict::Copy));
    form->addRow(tr("Connessioni già presenti"), conflict_);
    layout->addLayout(form);

    snippets_ = new QCheckBox(tr("Importa gli snippet (%n)", "", int(imported.snippets.size())), this);
    snippets_->setEnabled(!imported.snippets.isEmpty());
    snippets_->setChecked(!imported.snippets.isEmpty());
    layout->addWidget(snippets_);

    replaceAll_ = new QCheckBox(tr("Sostituisci l'intero archivio con il file (ripristino)"), this);
    replaceAll_->setToolTip(tr("Tutte le connessioni e gli snippet attuali vengono sostituiti da "
                               "quelli del file"));
    layout->addWidget(replaceAll_);
    replaceAll_->setVisible(allowReplace);
    snippets_->setVisible(allowReplace || !imported.snippets.isEmpty());

    summary_ = new QLabel(this);
    summary_->setWordWrap(true);
    layout->addWidget(summary_);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Importa"));
    connect(buttons, &QDialogButtonBox::accepted, this, &ImportSelectionDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    mergeWidgets_ = {picker_, destination_, conflict_, snippets_};
    for (int i = 0; i < buttonsRow->count(); ++i) {
        if (QWidget *widget = buttonsRow->itemAt(i)->widget())
            mergeWidgets_ << widget;
    }

    connect(picker_, &ConnectionPicker::selectionChanged, this, &ImportSelectionDialog::updateState);
    connect(replaceAll_, &QCheckBox::toggled, this, &ImportSelectionDialog::updateState);
    updateState();
}

bool ImportSelectionDialog::replaceAll() const
{
    return replaceAll_->isChecked();
}

QSet<QString> ImportSelectionDialog::hostIds() const
{
    QSet<QString> ids = picker_->checkedHostIds();
    archivetransfer::addJumpHosts(imported_, &ids);
    return ids;
}

QString ImportSelectionDialog::destinationId() const
{
    return destination_->currentData().toString();
}

archivetransfer::Conflict ImportSelectionDialog::conflict() const
{
    return archivetransfer::Conflict(conflict_->currentData().toInt());
}

bool ImportSelectionDialog::includeSnippets() const
{
    return snippets_->isEnabled() && snippets_->isChecked();
}

void ImportSelectionDialog::updateState()
{
    const bool replace = replaceAll_->isChecked();
    for (QWidget *widget : std::as_const(mergeWidgets_))
        widget->setEnabled(!replace);
    snippets_->setEnabled(!replace && !imported_.snippets.isEmpty());

    QVector<const Connection *> all;
    imported_.collectConnections(all);
    if (replace) {
        summary_->setText(tr("L'archivio attuale verrà sostituito con le %n connessione/i del file. "
                             "Le sessioni già aperte restano attive.",
                             "", int(all.size())));
        return;
    }
    QSet<QString> ids = picker_->checkedHostIds();
    const int chosen = int(ids.size());
    const int jumps = archivetransfer::addJumpHosts(imported_, &ids);
    int existing = 0;
    for (const QString &id : std::as_const(ids)) {
        if (current_.findConnection(id))
            ++existing;
    }
    QString text = tr("%n connessione/i selezionata/e", "", chosen);
    if (jumps > 0)
        text += tr(", più %n jump host necessario/i", "", jumps);
    if (existing > 0)
        text += tr("; %n già presente/i", "", existing);
    summary_->setText(text + QLatin1Char('.'));
}

void ImportSelectionDialog::accept()
{
    if (!replaceAll_->isChecked() && picker_->checkedHostIds().isEmpty()
        && !includeSnippets()) {
        QMessageBox::warning(this, windowTitle(), tr("Seleziona almeno una connessione."));
        return;
    }
    if (replaceAll_->isChecked()) {
        const auto answer = QMessageBox::question(
            this, windowTitle(),
            tr("Sostituire davvero l'intero archivio con il contenuto del file?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }
    QDialog::accept();
}
