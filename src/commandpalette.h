#pragma once

#include <QDialog>
#include <QIcon>
#include <QString>
#include <QVector>

class QAction;
class QLineEdit;
class QListWidget;

// One row of the command palette. Connections and actions are supplied by the
// main window; quick-connect rows are generated from the typed destination.
struct PaletteEntry {
    enum class Kind { Connection, RecentConnection, Action, QuickConnect };

    Kind kind = Kind::Connection;
    QString id;
    QString title;
    QString detail;
    QString shortcut;
    QString target;  // QuickConnect: user@host[:port]
    bool favorite = false;
    QIcon icon;
    QAction *action = nullptr;
};

// Modal fuzzy picker behind Ctrl+K. Typing filters connections, recent hosts
// and actions; a valid "user@host[:port]" also offers an ad-hoc session.
class CommandPalette : public QDialog {
    Q_OBJECT

public:
    explicit CommandPalette(QWidget *parent = nullptr);

    void setEntries(const QVector<PaletteEntry> &entries);
    PaletteEntry selectedEntry() const;

private:
    void rebuild();
    void chooseCurrent();

    QVector<PaletteEntry> entries_;
    QVector<PaletteEntry> shown_;
    QLineEdit *search_ = nullptr;
    QListWidget *list_ = nullptr;
};
