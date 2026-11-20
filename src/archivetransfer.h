#pragma once

#include "model.h"

#include <QDialog>
#include <QSet>
#include <QTreeWidget>

class QCheckBox;
class QComboBox;
class QLabel;

// Selective export and import of connections, e.g. to hand a few hosts to a
// colleague. The file format is the same as the full archive export.
namespace archivetransfer {

// Copy of the tree with only the given hosts and the folders leading to them.
Node subset(const Node &root, const QSet<QString> &hostIds);

// Adds the jump hosts the given hosts need, recursively; returns how many
// were added.
int addJumpHosts(const ConnectionStore &store, QSet<QString> *hostIds);

// Removes passwords, passphrases and embedded keys from every host.
void stripCredentials(Node &node);

enum class Conflict { Update, Skip, Copy };

struct MergeResult {
    int added = 0;
    int updated = 0;
    int skipped = 0;
    int copied = 0;
};

// Adds the selected hosts of `imported` under the folder `destinationId`,
// keeping their folder structure (folders with the same name are reused).
// A host whose id already exists is updated in place (credentials missing
// from the file are kept), skipped, or added as a copy with a new id.
MergeResult merge(ConnectionStore &store, const QString &destinationId, const Node &imported,
                  const QSet<QString> &hostIds, Conflict conflict);

} // namespace archivetransfer

// Tree of folders and hosts with check boxes; a folder checks everything
// inside it.
class ConnectionPicker : public QTreeWidget {
    Q_OBJECT

public:
    explicit ConnectionPicker(QWidget *parent = nullptr);

    // Empty `checked` means everything.
    void setRoot(const Node &root, const QSet<QString> &checked = {});
    QSet<QString> checkedHostIds() const;
    void setAllChecked(bool checked);

signals:
    void selectionChanged();

private:
    void addNode(QTreeWidgetItem *parent, const Node &node, const QSet<QString> &checked);
};

class ExportSelectionDialog : public QDialog {
    Q_OBJECT

public:
    ExportSelectionDialog(const ConnectionStore &store, const QSet<QString> &preselected,
                          QWidget *parent = nullptr);

    QSet<QString> hostIds() const; // jump hosts included
    bool includeCredentials() const;
    bool includeSnippets() const;
    void accept() override;

private:
    void updateSummary();

    const ConnectionStore &store_;
    ConnectionPicker *picker_ = nullptr;
    QCheckBox *credentials_ = nullptr;
    QCheckBox *snippets_ = nullptr;
    QLabel *summary_ = nullptr;
};

class ImportSelectionDialog : public QDialog {
    Q_OBJECT

public:
    // `allowReplace`: offer "replace the whole archive" (archive files only).
    ImportSelectionDialog(const ConnectionStore &current, const ConnectionStore &imported,
                          bool allowReplace = true, QWidget *parent = nullptr);

    bool replaceAll() const;
    QSet<QString> hostIds() const; // jump hosts included
    QString destinationId() const;
    archivetransfer::Conflict conflict() const;
    bool includeSnippets() const;
    void accept() override;

private:
    void updateState();

    const ConnectionStore &current_;
    const ConnectionStore &imported_;
    ConnectionPicker *picker_ = nullptr;
    QComboBox *destination_ = nullptr;
    QComboBox *conflict_ = nullptr;
    QCheckBox *snippets_ = nullptr;
    QCheckBox *replaceAll_ = nullptr;
    QLabel *summary_ = nullptr;
    QList<QWidget *> mergeWidgets_;
};
