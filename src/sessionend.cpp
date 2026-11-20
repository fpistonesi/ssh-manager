#include "sessionend.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QStringList>

namespace sessionend {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("SessionEnd", text);
}

struct Pattern {
    const char *needle;     // case-insensitive substring of ssh's message
    const char *meaning;    // translated explanation
};

// Most specific first: the first match in the latest lines wins.
const Pattern kPatterns[] = {
    {"REMOTE HOST IDENTIFICATION HAS CHANGED",
     QT_TRANSLATE_NOOP("SessionEnd", "La chiave del server è cambiata")},
    {"Host key verification failed",
     QT_TRANSLATE_NOOP("SessionEnd", "Chiave del server non verificata")},
    {"Permission denied",
     QT_TRANSLATE_NOOP("SessionEnd", "Autenticazione rifiutata")},
    {"Too many authentication failures",
     QT_TRANSLATE_NOOP("SessionEnd", "Troppi tentativi di autenticazione")},
    {"Could not resolve hostname",
     QT_TRANSLATE_NOOP("SessionEnd", "Nome host non risolto (DNS o VPN?)")},
    {"Connection refused",
     QT_TRANSLATE_NOOP("SessionEnd", "Connessione rifiutata: ssh non è in ascolto su quella porta")},
    {"Connection timed out",
     QT_TRANSLATE_NOOP("SessionEnd", "Server non raggiungibile (timeout)")},
    {"Network is unreachable",
     QT_TRANSLATE_NOOP("SessionEnd", "Rete non raggiungibile")},
    {"No route to host",
     QT_TRANSLATE_NOOP("SessionEnd", "Nessun percorso verso l'host")},
    {"not responding",
     QT_TRANSLATE_NOOP("SessionEnd", "Il server ha smesso di rispondere (connessione persa)")},
    {"Broken pipe",
     QT_TRANSLATE_NOOP("SessionEnd", "Connessione interrotta")},
    {"Connection reset",
     QT_TRANSLATE_NOOP("SessionEnd", "Connessione chiusa bruscamente dal server o dalla rete")},
    {"kex_exchange_identification",
     QT_TRANSLATE_NOOP("SessionEnd", "Il server ha chiuso la connessione prima del login")},
    {"Connection closed by",
     QT_TRANSLATE_NOOP("SessionEnd", "Connessione chiusa dal server")},
};

} // namespace

QString plainText(const QString &terminalOutput)
{
    QString text = terminalOutput;
    text.remove(QRegularExpression(QStringLiteral("\\x1b\\[[0-9;?]*[ -/]*[@-~]")));
    text.remove(QRegularExpression(QStringLiteral("\\x1b\\][^\\x07\\x1b]*(\\x07|\\x1b\\\\)")));
    text.remove(QRegularExpression(QStringLiteral("\\x1b[()][0-9A-Za-z]")));
    return text;
}

Reason describe(bool exitKnown, int exitCode, bool crashed, const QString &output, bool local)
{
    Reason reason;
    const QStringList lines = plainText(output).split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                                                      Qt::SkipEmptyParts);
    // Look at the last lines only: older ones belong to the session itself.
    const QStringList tail = lines.mid(qMax(0, int(lines.size()) - 12));

    if (!local) {
        for (const Pattern &pattern : kPatterns) {
            for (int i = int(tail.size()) - 1; i >= 0; --i) {
                if (tail.at(i).contains(QLatin1String(pattern.needle), Qt::CaseInsensitive)) {
                    reason.text = tr(pattern.meaning);
                    reason.detail = tail.at(i).trimmed();
                    break;
                }
            }
            if (!reason.text.isEmpty())
                break;
        }
    }

    if (crashed) {
        reason.unexpected = true;
        if (reason.text.isEmpty())
            reason.text = local ? tr("La shell locale è stata interrotta")
                                : tr("Il client ssh è stato interrotto");
        return reason;
    }

    if (!reason.text.isEmpty()) {
        // "Connection to host closed." after a normal logout is not an error.
        reason.unexpected = !(exitKnown && exitCode == 0);
        return reason;
    }

    if (!exitKnown) {
        reason.text = tr("Sessione terminata");
        return reason;
    }
    if (exitCode == 0) {
        reason.text = local ? tr("Shell chiusa") : tr("Disconnessione normale");
        return reason;
    }
    if (!local && exitCode == 255) {
        // ssh's own failures use 255; take the last line as explanation.
        reason.unexpected = true;
        reason.text = tr("Errore di ssh");
        if (!tail.isEmpty())
            reason.detail = tail.last().trimmed();
        return reason;
    }
    // Any other code is what the remote shell (or the last command) returned.
    reason.text = tr("La shell è uscita con codice %1").arg(exitCode);
    return reason;
}

} // namespace sessionend
