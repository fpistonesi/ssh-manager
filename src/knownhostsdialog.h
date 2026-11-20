#pragma once

#include <QDialog>
#include <QPair>
#include <QVector>

class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

// Asks the user to trust a host key, from the prompt OpenSSH passes to the
// askpass helper ("The authenticity of host ... can't be established ...").
// Returns true when the key is accepted.
bool confirmHostKey(const QString &prompt);

// True for the host key confirmation prompt, as opposed to a password or
// passphrase request.
bool isHostKeyPrompt(const QString &prompt);

// Lists and edits the known_hosts file used by SSH Manager.
class KnownHostsDialog : public QDialog {
    Q_OBJECT

public:
    // `connections`: saved connection names with the known_hosts name
    // ("host" or "[host]:port") they use, to show which ones a key serves.
    KnownHostsDialog(const QVector<QPair<QString, QString>> &connections,
                     const QString &filter = QString(), QWidget *parent = nullptr);

private:
    void reload();
    void applyFilter();
    void removeSelected();
    void copyFingerprint();
    void importOpenSsh();
    void updateButtons();

    QVector<QPair<QString, QString>> connections_;
    QLabel *pathLabel_ = nullptr;
    QLineEdit *filter_ = nullptr;
    QTreeWidget *list_ = nullptr;
    QPushButton *removeButton_ = nullptr;
    QPushButton *copyButton_ = nullptr;
};
