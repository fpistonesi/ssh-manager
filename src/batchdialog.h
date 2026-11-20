#pragma once

#include "model.h"
#include "remoterun.h"

#include <QDateTime>
#include <QDialog>
#include <QHash>
#include <functional>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTreeWidget;
class QTreeWidgetItem;

// Runs one command on several hosts in parallel and collects, per host, the
// output, the exit code and the duration.
class BatchDialog : public QDialog {
    Q_OBJECT

public:
    struct Host {
        QString id;
        QString name;
        Connection connection; // sanitized: names and ${variables} only
    };
    // Builds the plan of one host (may ask for a password); an invalid plan
    // carries the error.
    using PlanFactory = std::function<SshPlan(const QString &id)>;

    BatchDialog(const QVector<Host> &hosts, const QStringList &preselected,
                const QVector<Snippet> &snippets, PlanFactory planFactory,
                QWidget *parent = nullptr);
    ~BatchDialog() override;

protected:
    void reject() override;

private:
    struct Run {
        QString id;
        QTreeWidgetItem *row = nullptr;
        RemoteRun *process = nullptr;
        QStringList temporaryFiles;
        RemoteRun::Result result;
        bool done = false;
        bool started = false;
        QString expandedCommand;
    };

    void start(const QStringList &ids);
    void launchNext();
    void onFinished(const QString &id, const RemoteRun::Result &result);
    void stopAll();
    void showOutput();
    void updateSummary();
    void save();
    QStringList checkedIds() const;
    const Host *host(const QString &id) const;

    QVector<Host> hosts_;
    QVector<Snippet> snippets_;
    PlanFactory planFactory_;
    QHash<QString, QString> variableValues_;

    QTreeWidget *hostList_ = nullptr;
    QPlainTextEdit *command_ = nullptr;
    QComboBox *snippetCombo_ = nullptr;
    QSpinBox *parallel_ = nullptr;
    QSpinBox *timeout_ = nullptr;
    QPushButton *runButton_ = nullptr;
    QPushButton *stopButton_ = nullptr;
    QPushButton *rerunButton_ = nullptr;
    QPushButton *saveButton_ = nullptr;
    QTreeWidget *results_ = nullptr;
    QPlainTextEdit *output_ = nullptr;
    QLabel *summary_ = nullptr;

    QVector<Run> runs_;
    QString lastCommand_;
    QDateTime startedAt_;
    int maxParallel_ = 4;
    int timeoutMs_ = 60000;
};
