#pragma once

#include "model.h"

#include <QHash>
#include <QString>
#include <QStringList>

namespace snippets {

// Fills ${host}, ${user}, ${port} and ${name} from the connection.
QString expandBuiltins(const QString &text, const Connection &connection);

// Other ${variables} of the text, in order of appearance, without repeats.
QStringList customVariables(const QString &text);

// Replaces ${variable} with the given values; unknown ones stay as they are.
QString applyVariables(const QString &text, const QHash<QString, QString> &values);

// The command typed after a shell prompt, from the screen line the cursor is
// on ("user@host:~$ ls -la" -> "ls -la"). Empty when the line does not look
// like a prompt, or when the command starts with a space (the bash/zsh
// convention for "do not record this").
QString commandFromPromptLine(const QString &line);

} // namespace snippets
