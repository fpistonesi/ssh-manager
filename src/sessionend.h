#pragma once

#include <QString>

namespace sessionend {

struct Reason {
    QString text;         // readable explanation, in the UI language
    QString detail;       // the ssh line it comes from, if any
    bool unexpected = false; // not a normal logout
};

// Why an ssh session ended, from the exit code (when known) and the last
// lines it printed. `local`: a local shell rather than ssh.
Reason describe(bool exitKnown, int exitCode, bool crashed, const QString &output, bool local);

// Removes terminal control sequences, so that messages can be matched.
QString plainText(const QString &terminalOutput);

} // namespace sessionend
