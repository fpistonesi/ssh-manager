#include "apptheme.h"
#include "knownhostsdialog.h"
#include "mainwindow.h"
#include "paths.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibraryInfo>
#include <QLocale>
#include <QSettings>
#include <QStringList>
#include <QStyleHints>
#include <QTimer>
#include <QTranslator>

#include <cstdio>

namespace {

// OpenSSH calls this executable through SSH_ASKPASS for password and key
// passphrase prompts. The matching entry is chosen using the host contained in
// the prompt, so jump hosts and the final destination can have different
// credentials.
int runAskpass(const QStringList &arguments)
{
    const QString prompt = arguments.join(QLatin1Char(' '));
    const QString path = qEnvironmentVariable("SSHMANAGER_ASKPASS_FILE");

    QString secret;
    if (!path.isEmpty()) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            const QJsonArray entries = QJsonDocument::fromJson(file.readAll())
                                           .object()
                                           .value(QStringLiteral("entries"))
                                           .toArray();

            const bool wantPassphrase =
                prompt.contains(QStringLiteral("passphrase"), Qt::CaseInsensitive)
                || prompt.contains(QStringLiteral("chiave"), Qt::CaseInsensitive)
                || prompt.contains(QStringLiteral(" key "), Qt::CaseInsensitive);

            QJsonObject chosen;
            for (const QJsonValue &value : entries) {
                const QJsonObject entry = value.toObject();
                const QString host = entry.value(QStringLiteral("host")).toString();
                if (!host.isEmpty() && prompt.contains(host, Qt::CaseInsensitive)) {
                    chosen = entry;
                    break;
                }
            }
            if (chosen.isEmpty() && !entries.isEmpty())
                chosen = entries.first().toObject();

            secret = chosen.value(wantPassphrase ? QStringLiteral("passphrase")
                                                 : QStringLiteral("password"))
                         .toString();
        }
    }

    const QByteArray output = secret.toUtf8() + '\n';
    if (!output.isEmpty())
        fwrite(output.constData(), 1, size_t(output.size()), stdout);
    fflush(stdout);
    return 0;
}

// Names and translations shared by the main window and the askpass dialogs.
void setupApplication(QApplication &app)
{
    QApplication::setApplicationName(QStringLiteral("sshmanager"));
    QApplication::setApplicationDisplayName(QStringLiteral("SSH Manager"));
    QApplication::setOrganizationName(QStringLiteral("sshmanager"));

    // Portable mode (vault.db next to the executable): settings go to
    // <folder>/sshmanager/sshmanager.ini instead of the registry or ~/.config.
    if (paths::isPortable()) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, paths::portableDir());
    }

    QSettings settings;
    // Light / dark / system theme, before any window exists (the askpass
    // dialogs share it).
    apptheme::setMode(settings.value(QStringLiteral("view/theme"), QStringLiteral("system")).toString());

    QString language = settings.value(QStringLiteral("ui/language")).toString();
    if (language != QLatin1String("it") && language != QLatin1String("en")) {
        language = QLocale::system().language() == QLocale::Italian
                       ? QStringLiteral("it")
                       : QStringLiteral("en");
    }
    // Qt's own strings (standard buttons, dialogs) follow the same language.
    // windeployqt merges them into qt_<lang>.qm next to the executable; a Qt
    // installation has qtbase_<lang>.qm as well.
    auto *qtTranslator = new QTranslator(&app);
    const QString qtTranslations = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
    if (qtTranslator->load(QStringLiteral("qt_%1").arg(language), qtTranslations)
        || qtTranslator->load(QStringLiteral("qtbase_%1").arg(language), qtTranslations))
        app.installTranslator(qtTranslator);

    auto *translator = new QTranslator(&app);
    if (translator->load(QStringLiteral(":/i18n/sshmanager_%1.qm").arg(language)))
        app.installTranslator(translator);
}

bool hasAskpassFlag(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--askpass") == 0)
            return true;
    }
    return false;
}

} // namespace

int main(int argc, char *argv[])
{
    // OpenSSH runs SSH_ASKPASS with only the prompt as argument, so the helper
    // mode is also detected through the environment variable we pass to ssh.
    const bool askpassMode =
        hasAskpassFlag(argc, argv)
        || (argc == 2 && !qEnvironmentVariableIsEmpty("SSHMANAGER_ASKPASS_FILE"));

    if (askpassMode) {
        QStringList promptArguments;
        for (int i = 1; i < argc; ++i) {
            if (qstrcmp(argv[i], "--askpass") != 0)
                promptArguments << QString::fromLocal8Bit(argv[i]);
        }
        const QString prompt = promptArguments.join(QLatin1Char(' '));

        // Host key confirmation: show the fingerprint and let the user decide.
        if (isHostKeyPrompt(prompt)) {
            QApplication app(argc, argv);
            setupApplication(app);
            const QByteArray answer = confirmHostKey(prompt) ? "yes\n" : "no\n";
            fwrite(answer.constData(), 1, size_t(answer.size()), stdout);
            fflush(stdout);
            return 0;
        }

        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("sshmanager"));
        return runAskpass(promptArguments);
    }

    QApplication app(argc, argv);
    setupApplication(app);

    MainWindow window;
    window.show();

    // Packaging check (packaging/windows/package.ps1): once the event loop
    // runs, write what was loaded and quit. Nothing is saved.
    const QString probe = qEnvironmentVariable("SSHMANAGER_STARTUP_PROBE");
    if (!probe.isEmpty()) {
        QTimer::singleShot(500, &app, [probe] {
            QFile file(probe);
            if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
                const QString qtSample = QCoreApplication::translate("QPlatformTheme", "Cancel");
                file.write(QStringLiteral("started\nqt-translation-sample=%1\nportable=%2\nsettings=%3\n")
                               .arg(qtSample, paths::isPortable() ? QStringLiteral("yes") : QStringLiteral("no"),
                                    QSettings().fileName())
                               .toUtf8());
            }
            QCoreApplication::exit(0);
        });
    }
    return app.exec();
}
