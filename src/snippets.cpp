#include "snippets.h"

#include <QRegularExpression>

namespace snippets {

namespace {

const QRegularExpression &variablePattern()
{
    static const QRegularExpression re(QStringLiteral("\\$\\{([A-Za-z_][A-Za-z0-9_]*)\\}"));
    return re;
}

bool isBuiltin(const QString &name)
{
    return name == QLatin1String("host") || name == QLatin1String("user")
           || name == QLatin1String("port") || name == QLatin1String("name");
}

} // namespace

QString applyVariables(const QString &text, const QHash<QString, QString> &values)
{
    QString result;
    qsizetype last = 0;
    QRegularExpressionMatchIterator it = variablePattern().globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        result += text.mid(last, match.capturedStart() - last);
        const QString name = match.captured(1);
        result += values.contains(name) ? values.value(name) : match.captured(0);
        last = match.capturedEnd();
    }
    result += text.mid(last);
    return result;
}

QString expandBuiltins(const QString &text, const Connection &connection)
{
    QHash<QString, QString> values;
    if (connection.local) {
        values.insert(QStringLiteral("host"), QStringLiteral("localhost"));
        values.insert(QStringLiteral("user"),
                      qEnvironmentVariable("USER", qEnvironmentVariable("USERNAME")));
        values.insert(QStringLiteral("port"), QString());
    } else {
        values.insert(QStringLiteral("host"), connection.host);
        values.insert(QStringLiteral("user"), connection.user);
        values.insert(QStringLiteral("port"), QString::number(connection.port));
    }
    values.insert(QStringLiteral("name"), connection.displayName());
    return applyVariables(text, values);
}

QStringList customVariables(const QString &text)
{
    QStringList names;
    QRegularExpressionMatchIterator it = variablePattern().globalMatch(text);
    while (it.hasNext()) {
        const QString name = it.next().captured(1);
        if (!isBuiltin(name) && !names.contains(name))
            names << name;
    }
    return names;
}

QString commandFromPromptLine(const QString &line)
{
    // The first prompt terminator followed by a space ends the prompt:
    // "user@host:~/dir$ ", "[root@host ~]# ", "host% ", "❯ ", "PS C:\> ".
    static const QRegularExpression prompt(QStringLiteral("[$#%>\\x{276F}\\x{279C}] "));
    const QRegularExpressionMatch match = prompt.match(line);
    if (!match.hasMatch())
        return QString();
    const QString rest = line.mid(match.capturedEnd());
    // A leading space (beyond the one after the prompt) means "keep it out
    // of the history", as with HISTCONTROL=ignorespace.
    if (rest.startsWith(QLatin1Char(' ')))
        return QString();
    const QString command = rest.trimmed();
    if (command.size() > 2000)
        return QString();
    return command;
}

} // namespace snippets
