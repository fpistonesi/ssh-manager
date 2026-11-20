#pragma once

#include "model.h"

#include <QDialog>
#include <QString>
#include <QStringList>
#include <QVector>

class QComboBox;
class QLineEdit;
class QListWidget;
class QPushButton;

// Programs launched against a connection (ping, tracert, ftp, a browser...).
// They belong to the application settings, not to the vault: no secrets.
struct ExternalTool {
    enum class Mode { Terminal, Detached, Url };

    QString name;
    // Program and arguments, or a URL; ${host}, ${port}, ${user}, ${name}
    // come from the connection, any other ${variable} is asked at launch.
    QString command;
    Mode mode = Mode::Terminal;
};

namespace externaltools {

QVector<ExternalTool> load();
void save(const QVector<ExternalTool> &tools);
QVector<ExternalTool> defaults();

// The command split into program and arguments, with the connection's
// values filled in token by token (a value with spaces stays one argument).
// The program is resolved through PATH when possible.
QStringList expandCommand(const QString &command, const Connection &connection,
                          const QHash<QString, QString> &custom);

} // namespace externaltools

class ExternalToolsDialog : public QDialog {
    Q_OBJECT

public:
    explicit ExternalToolsDialog(QWidget *parent = nullptr);

    QVector<ExternalTool> tools() const { return tools_; }
    void accept() override;

private:
    void rebuildList(int select);
    void showTool(int row);
    void storeCurrent();
    void browseProgram();

    QVector<ExternalTool> tools_;
    int current_ = -1;
    bool loading_ = false;

    QListWidget *list_ = nullptr;
    QLineEdit *name_ = nullptr;
    QLineEdit *command_ = nullptr;
    QComboBox *mode_ = nullptr;
    QPushButton *browse_ = nullptr;
    QPushButton *remove_ = nullptr;
    QPushButton *up_ = nullptr;
    QPushButton *down_ = nullptr;
};
