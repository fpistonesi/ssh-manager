#pragma once

#include "remoterun.h"

#include <QDialog>
#include <QStringList>
#include <QVector>

class QCheckBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QTimer;
class QTreeWidget;

namespace hosthealth {

// Read-only script run with "sh -s"; every section starts with a "@@name"
// line so that a missing tool only leaves its section empty.
QByteArray script();

struct Disk {
    QString mount;
    QString device;
    qint64 sizeKb = 0;
    qint64 usedKb = 0;
    int percent = 0;
};
struct Process {
    QString pid;
    QString user;
    double cpu = 0;
    double mem = 0;
    QString command;
};
struct Snapshot {
    QString os;
    QString kernel;
    QString hostname;
    qint64 uptimeSeconds = -1;
    double load1 = -1, load5 = -1, load15 = -1;
    int cpus = 0;
    qint64 memTotalKb = 0, memAvailableKb = 0, swapTotalKb = 0, swapFreeKb = 0;
    QVector<Disk> disks;
    QStringList failedUnits;
    bool systemd = false;
    QVector<Process> processes;
};

Snapshot parse(const QByteArray &output);

} // namespace hosthealth

// Dashboard of one host: load, memory, disks, failed systemd units and the
// busiest processes, from a single ssh run. Owns the plan's temporary files.
class HostHealthDialog : public QDialog {
    Q_OBJECT

public:
    HostHealthDialog(const QString &name, const QString &target, const SshPlan &plan,
                     QWidget *parent = nullptr);
    ~HostHealthDialog() override;

private:
    void refresh();
    void onFinished(const RemoteRun::Result &result);
    void display(const hosthealth::Snapshot &snapshot);

    SshPlan plan_;
    RemoteRun *run_ = nullptr;
    QTimer *autoTimer_ = nullptr;

    QLabel *status_ = nullptr;
    QLabel *system_ = nullptr;
    QLabel *uptime_ = nullptr;
    QLabel *load_ = nullptr;
    QProgressBar *loadBar_ = nullptr;
    QProgressBar *memBar_ = nullptr;
    QLabel *mem_ = nullptr;
    QProgressBar *swapBar_ = nullptr;
    QLabel *swap_ = nullptr;
    QTreeWidget *disks_ = nullptr;
    QTreeWidget *failed_ = nullptr;
    QTreeWidget *processes_ = nullptr;
    QPushButton *refreshButton_ = nullptr;
    QCheckBox *auto_ = nullptr;
};
