#include "hosthealth.h"

#include "apptheme.h"

#include <QCheckBox>
#include <QFile>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSplitter>
#include <QStyle>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace hosthealth {

QByteArray script()
{
    // POSIX sh only; every tool may be missing (busybox, containers, BSD).
    return QByteArrayLiteral(
        "LC_ALL=C; export LC_ALL\n"
        "echo @@os\n"
        "( . /etc/os-release 2>/dev/null && echo \"$PRETTY_NAME\" ) || uname -s\n"
        "uname -r 2>/dev/null\n"
        "hostname 2>/dev/null || uname -n\n"
        "echo @@uptime\n"
        "cat /proc/uptime 2>/dev/null\n"
        "echo @@load\n"
        "cat /proc/loadavg 2>/dev/null\n"
        "echo @@cpus\n"
        "nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || grep -c ^processor /proc/cpuinfo 2>/dev/null\n"
        "echo @@mem\n"
        "grep -E '^(MemTotal|MemAvailable|MemFree|Buffers|Cached|SwapTotal|SwapFree):' /proc/meminfo 2>/dev/null\n"
        "echo @@disk\n"
        "df -P -k -x tmpfs -x devtmpfs -x squashfs -x overlay -x efivarfs 2>/dev/null || df -P -k 2>/dev/null\n"
        "echo @@failed\n"
        "if command -v systemctl >/dev/null 2>&1; then echo systemd; "
        "systemctl --failed --no-legend --plain 2>/dev/null; fi\n"
        "echo @@top\n"
        // Leave out this script and its children (ps, awk, head): just
        // started, they show meaningless CPU figures.
        "ps -eo pid,ppid,user,pcpu,pmem,comm --sort=-pcpu 2>/dev/null"
        " | awk -v self=$$ 'NR == 1 || ($1 != self && $2 != self)' | head -n 11\n"
        "echo @@end\n");
}

Snapshot parse(const QByteArray &output)
{
    Snapshot s;
    QString section;
    QStringList os;
    qint64 memFree = -1, buffers = 0, cached = 0;
    bool firstTopLine = true;
    const QRegularExpression spaces(QStringLiteral("\\s+"));

    for (const QString &raw : QString::fromUtf8(output).split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1String("@@"))) {
            section = line.mid(2);
            continue;
        }
        if (line.isEmpty())
            continue;
        const QStringList f = line.split(spaces, Qt::SkipEmptyParts);

        if (section == QLatin1String("os")) {
            os << line;
        } else if (section == QLatin1String("uptime")) {
            s.uptimeSeconds = qint64(f.value(0).toDouble());
        } else if (section == QLatin1String("load")) {
            s.load1 = f.value(0).toDouble();
            s.load5 = f.value(1).toDouble();
            s.load15 = f.value(2).toDouble();
        } else if (section == QLatin1String("cpus")) {
            s.cpus = qMax(s.cpus, f.value(0).toInt());
        } else if (section == QLatin1String("mem")) {
            const qint64 kb = f.value(1).toLongLong();
            const QString key = f.value(0);
            if (key == QLatin1String("MemTotal:")) s.memTotalKb = kb;
            else if (key == QLatin1String("MemAvailable:")) s.memAvailableKb = kb;
            else if (key == QLatin1String("MemFree:")) memFree = kb;
            else if (key == QLatin1String("Buffers:")) buffers = kb;
            else if (key == QLatin1String("Cached:")) cached = kb;
            else if (key == QLatin1String("SwapTotal:")) s.swapTotalKb = kb;
            else if (key == QLatin1String("SwapFree:")) s.swapFreeKb = kb;
        } else if (section == QLatin1String("disk")) {
            // Filesystem 1024-blocks Used Available Capacity Mounted-on
            if (f.size() < 6 || f.at(0) == QLatin1String("Filesystem"))
                continue;
            Disk d;
            d.device = f.at(0);
            d.sizeKb = f.at(1).toLongLong();
            d.usedKb = f.at(2).toLongLong();
            d.percent = QString(f.at(4)).remove(QLatin1Char('%')).toInt();
            d.mount = f.mid(5).join(QLatin1Char(' '));
            if (d.sizeKb > 0)
                s.disks.append(d);
        } else if (section == QLatin1String("failed")) {
            if (line == QLatin1String("systemd"))
                s.systemd = true;
            else
                s.failedUnits << f.value(0) + (f.size() > 4 ? QStringLiteral(" — ") + f.mid(4).join(QLatin1Char(' ')) : QString());
        } else if (section == QLatin1String("top")) {
            if (firstTopLine) { // header
                firstTopLine = false;
                continue;
            }
            // pid ppid user %cpu %mem command
            if (f.size() < 6)
                continue;
            Process p;
            p.pid = f.at(0);
            p.user = f.at(2);
            p.cpu = f.at(3).toDouble();
            p.mem = f.at(4).toDouble();
            p.command = f.mid(5).join(QLatin1Char(' '));
            s.processes.append(p);
        }
    }
    s.os = os.value(0);
    s.kernel = os.value(1);
    s.hostname = os.value(2);
    if (s.memAvailableKb == 0 && memFree >= 0) // kernels before 3.14
        s.memAvailableKb = memFree + buffers + cached;
    return s;
}

} // namespace hosthealth

namespace {

QString kb(qint64 kilobytes)
{
    const double gib = kilobytes / (1024.0 * 1024.0);
    if (gib >= 1)
        return QStringLiteral("%1 GB").arg(QString::number(gib, 'f', gib < 10 ? 1 : 0));
    return QStringLiteral("%1 MB").arg(QString::number(kilobytes / 1024.0, 'f', 0));
}

// Green, amber, red, like the rest of the app.
void colorBar(QProgressBar *bar, int percent)
{
    const QColor color = percent >= 90 ? apptheme::danger()
                                       : (percent >= 75 ? apptheme::warning() : apptheme::success());
    bar->setStyleSheet(QStringLiteral("QProgressBar::chunk { background: %1; }").arg(color.name()));
}

} // namespace

HostHealthDialog::HostHealthDialog(const QString &name, const QString &target, const SshPlan &plan,
                                   QWidget *parent)
    : QDialog(parent), plan_(plan)
{
    setWindowTitle(tr("Stato di %1").arg(name));
    setWindowFlag(Qt::WindowMaximizeButtonHint, true);
    resize(960, 720);

    auto *layout = new QVBoxLayout(this);
    auto *header = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("<b>%1</b> — %2").arg(name.toHtmlEscaped(), target.toHtmlEscaped()), this);
    status_ = new QLabel(this);
    status_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto_ = new QCheckBox(tr("Aggiorna ogni 30 s"), this);
    refreshButton_ = new QPushButton(style()->standardIcon(QStyle::SP_BrowserReload), tr("Aggiorna"), this);
    header->addWidget(title);
    header->addWidget(status_, 1);
    header->addWidget(auto_);
    header->addWidget(refreshButton_);
    layout->addLayout(header);

    // --- Summary cards -------------------------------------------------------
    auto *summary = new QGroupBox(tr("Sistema"), this);
    auto *grid = new QGridLayout(summary);
    system_ = new QLabel(summary);
    system_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    uptime_ = new QLabel(summary);
    load_ = new QLabel(summary);
    loadBar_ = new QProgressBar(summary);
    mem_ = new QLabel(summary);
    memBar_ = new QProgressBar(summary);
    swap_ = new QLabel(summary);
    swapBar_ = new QProgressBar(summary);
    for (QProgressBar *bar : {loadBar_, memBar_, swapBar_}) {
        bar->setRange(0, 100);
        bar->setTextVisible(true);
        bar->setMaximumHeight(16);
    }
    grid->addWidget(system_, 0, 0, 1, 3);
    grid->addWidget(new QLabel(tr("Uptime"), summary), 1, 0);
    grid->addWidget(uptime_, 1, 1, 1, 2);
    grid->addWidget(new QLabel(tr("Carico"), summary), 2, 0);
    grid->addWidget(loadBar_, 2, 1);
    grid->addWidget(load_, 2, 2);
    grid->addWidget(new QLabel(tr("Memoria"), summary), 3, 0);
    grid->addWidget(memBar_, 3, 1);
    grid->addWidget(mem_, 3, 2);
    grid->addWidget(new QLabel(tr("Swap"), summary), 4, 0);
    grid->addWidget(swapBar_, 4, 1);
    grid->addWidget(swap_, 4, 2);
    grid->setColumnStretch(1, 1);
    layout->addWidget(summary);

    auto *split = new QSplitter(Qt::Vertical, this);
    const auto makeList = [split](const QString &title, const QStringList &columns) {
        auto *box = new QGroupBox(title, split);
        auto *boxLayout = new QVBoxLayout(box);
        auto *list = new QTreeWidget(box);
        list->setHeaderLabels(columns);
        list->setRootIsDecorated(false);
        list->setUniformRowHeights(true);
        list->setAlternatingRowColors(true);
        boxLayout->addWidget(list);
        return list;
    };
    disks_ = makeList(tr("Dischi"), {tr("Punto di montaggio"), tr("Uso"), tr("Usato"), tr("Totale"), tr("Dispositivo")});
    disks_->header()->resizeSection(0, 220);
    disks_->header()->resizeSection(1, 200);
    failed_ = makeList(tr("Servizi systemd non riusciti"), {tr("Unità")});
    processes_ = makeList(tr("Processi più attivi"), {tr("PID"), tr("Utente"), tr("CPU %"), tr("Mem %"), tr("Comando")});
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 1);
    split->setStretchFactor(2, 2);
    layout->addWidget(split, 1);

    autoTimer_ = new QTimer(this);
    autoTimer_->setInterval(30000);
    connect(autoTimer_, &QTimer::timeout, this, &HostHealthDialog::refresh);
    connect(auto_, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("health/autoRefresh"), on);
        on ? autoTimer_->start() : autoTimer_->stop();
    });
    connect(refreshButton_, &QPushButton::clicked, this, &HostHealthDialog::refresh);
    auto_->setChecked(QSettings().value(QStringLiteral("health/autoRefresh"), false).toBool());

    refresh();
}

HostHealthDialog::~HostHealthDialog()
{
    delete run_;
    for (const QString &path : plan_.temporaryFiles)
        QFile::remove(path);
}

void HostHealthDialog::refresh()
{
    if (run_ && run_->isRunning())
        return;
    delete run_;
    status_->setText(tr("Lettura in corso…"));
    refreshButton_->setEnabled(false);
    run_ = new RemoteRun(plan_, QStringLiteral("sh -s"), hosthealth::script(), 30000, this);
    connect(run_, &RemoteRun::finished, this, &HostHealthDialog::onFinished);
    run_->start();
}

void HostHealthDialog::onFinished(const RemoteRun::Result &result)
{
    refreshButton_->setEnabled(true);
    const bool parsed = result.out.contains("@@end");
    if (!parsed) {
        QString error = result.startError;
        if (error.isEmpty())
            error = result.timedOut ? tr("Nessuna risposta entro 30 s")
                                    : QString::fromUtf8(result.err).trimmed().section(QLatin1Char('\n'), -1);
        if (error.isEmpty())
            error = tr("Il server non ha restituito i dati (codice %1)").arg(result.exitCode);
        status_->setText(QStringLiteral("<span style=\"color:%1\">%2</span>")
                             .arg(apptheme::danger().name(), error.toHtmlEscaped()));
        return;
    }
    display(hosthealth::parse(result.out));
    status_->setText(tr("Aggiornato alle %1 (%2 ms)")
                         .arg(QLocale().toString(QTime::currentTime(), QLocale::ShortFormat))
                         .arg(result.elapsedMs));
}

void HostHealthDialog::display(const hosthealth::Snapshot &s)
{
    QStringList system;
    if (!s.hostname.isEmpty())
        system << QStringLiteral("<b>%1</b>").arg(s.hostname.toHtmlEscaped());
    if (!s.os.isEmpty())
        system << s.os.toHtmlEscaped();
    if (!s.kernel.isEmpty())
        system << tr("kernel %1").arg(s.kernel.toHtmlEscaped());
    if (s.cpus > 0)
        system << tr("CPU: %1").arg(s.cpus);
    system_->setText(system.join(QStringLiteral(" · ")));

    if (s.uptimeSeconds >= 0) {
        const qint64 days = s.uptimeSeconds / 86400;
        const qint64 hours = (s.uptimeSeconds % 86400) / 3600;
        const qint64 minutes = (s.uptimeSeconds % 3600) / 60;
        uptime_->setText(days > 0 ? tr("%1 giorni, %2 h %3 min").arg(days).arg(hours).arg(minutes)
                                  : tr("%1 h %2 min").arg(hours).arg(minutes));
    } else {
        uptime_->setText(QStringLiteral("—"));
    }

    if (s.load1 >= 0) {
        // Load relative to the cores: 100% means every core busy on average.
        const int percent = s.cpus > 0 ? int(s.load1 * 100 / s.cpus) : 0;
        loadBar_->setValue(qMin(percent, 100));
        loadBar_->setFormat(s.cpus > 0 ? QStringLiteral("%1%").arg(percent) : QStringLiteral("—"));
        colorBar(loadBar_, percent);
        load_->setText(tr("%1 · %2 · %3 (1, 5, 15 min)")
                           .arg(s.load1, 0, 'f', 2)
                           .arg(s.load5, 0, 'f', 2)
                           .arg(s.load15, 0, 'f', 2));
    }

    if (s.memTotalKb > 0) {
        const qint64 used = s.memTotalKb - s.memAvailableKb;
        const int percent = int(used * 100 / s.memTotalKb);
        memBar_->setValue(percent);
        colorBar(memBar_, percent);
        mem_->setText(tr("%1 usati di %2").arg(kb(used), kb(s.memTotalKb)));
    }
    if (s.swapTotalKb > 0) {
        const qint64 used = s.swapTotalKb - s.swapFreeKb;
        const int percent = int(used * 100 / s.swapTotalKb);
        swapBar_->setValue(percent);
        colorBar(swapBar_, percent);
        swap_->setText(tr("%1 usati di %2").arg(kb(used), kb(s.swapTotalKb)));
    } else {
        swapBar_->setValue(0);
        swap_->setText(tr("nessuno swap"));
    }

    disks_->clear();
    for (const hosthealth::Disk &d : s.disks) {
        auto *item = new QTreeWidgetItem(disks_);
        item->setText(0, d.mount);
        item->setText(2, kb(d.usedKb));
        item->setText(3, kb(d.sizeKb));
        item->setText(4, d.device);
        auto *bar = new QProgressBar(disks_);
        bar->setRange(0, 100);
        bar->setValue(d.percent);
        bar->setFormat(QStringLiteral("%1%").arg(d.percent));
        bar->setMaximumHeight(16);
        colorBar(bar, d.percent);
        disks_->setItemWidget(item, 1, bar);
    }

    failed_->clear();
    if (!s.systemd) {
        failed_->addTopLevelItem(new QTreeWidgetItem({tr("systemd non presente su questo host")}));
    } else if (s.failedUnits.isEmpty()) {
        auto *item = new QTreeWidgetItem({tr("Nessun servizio in errore")});
        item->setForeground(0, apptheme::success());
        failed_->addTopLevelItem(item);
    } else {
        for (const QString &unit : s.failedUnits) {
            auto *item = new QTreeWidgetItem({unit});
            item->setForeground(0, apptheme::danger());
            failed_->addTopLevelItem(item);
        }
    }

    processes_->clear();
    for (const hosthealth::Process &p : s.processes) {
        auto *item = new QTreeWidgetItem(processes_);
        item->setText(0, p.pid);
        item->setText(1, p.user);
        item->setText(2, QString::number(p.cpu, 'f', 1));
        item->setText(3, QString::number(p.mem, 'f', 1));
        item->setText(4, p.command);
        item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        item->setTextAlignment(3, Qt::AlignRight | Qt::AlignVCenter);
    }
}
