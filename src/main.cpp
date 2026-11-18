#include "mainwindow.h"

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
        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("sshmanager"));

        QStringList promptArguments;
        for (int i = 1; i < argc; ++i) {
            if (qstrcmp(argv[i], "--askpass") != 0)
                promptArguments << QString::fromLocal8Bit(argv[i]);
        }
        return runAskpass(promptArguments);
    }

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("sshmanager"));
    QApplication::setApplicationDisplayName(QStringLiteral("SSH Manager"));
    QApplication::setOrganizationName(QStringLiteral("sshmanager"));

    QSettings settings;
    QString language = settings.value(QStringLiteral("ui/language")).toString();
    if (language != QLatin1String("it") && language != QLatin1String("en")) {
        language = QLocale::system().language() == QLocale::Italian
                       ? QStringLiteral("it")
                       : QStringLiteral("en");
    }
    // Qt's own strings (standard buttons, dialogs) follow the same language.
    auto *qtTranslator = new QTranslator(&app);
    if (qtTranslator->load(QStringLiteral("qtbase_%1").arg(language),
                           QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        app.installTranslator(qtTranslator);

    auto *translator = new QTranslator(&app);
    if (translator->load(QStringLiteral(":/i18n/sshmanager_%1.qm").arg(language)))
        app.installTranslator(translator);

    MainWindow window;
    window.show();
    return app.exec();
}
