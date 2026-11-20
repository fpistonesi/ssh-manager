#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

// Reading and editing an OpenSSH known_hosts file, plus the "first seen"
// dates SSH Manager records next to it (OpenSSH keeps no dates).
namespace knownhosts {

struct Entry {
    int line = 0;           // 0-based line number in the file
    QString marker;         // "@cert-authority" / "@revoked", usually empty
    QStringList hosts;      // patterns as written ("host", "[host]:port", "|1|...")
    bool hashed = false;    // HashKnownHosts entry: the names are not readable
    QString keyType;        // "ssh-ed25519", "ecdsa-sha2-nistp256", ...
    QByteArray key;         // decoded key blob
    QString comment;
    QString fingerprint;    // "SHA256:..."
};

// Parses every key line; comments and malformed lines are skipped (and kept
// untouched by the editing functions).
QVector<Entry> read(const QString &path, QString *error = nullptr);

// "SHA256:<base64 without padding>", like ssh-keygen -l.
QString fingerprint(const QByteArray &keyBlob);

// The name ssh writes and looks up: "host" on port 22, "[host]:port" otherwise.
QString hostName(const QString &host, quint16 port);

// True when the entry is for `name` (a hostName() value), hashed or not.
bool matches(const Entry &entry, const QString &name);

// Rewrites the file without the given lines. Returns false on I/O errors.
bool removeLines(const QString &path, const QSet<int> &lines, QString *error = nullptr);

// Removes every entry for `name`; returns how many lines were dropped, -1 on error.
int removeHost(const QString &path, const QString &name, QString *error = nullptr);

// Appends the key lines of `source` whose host and key are not in `path` yet;
// returns how many were added, -1 on error.
int import(const QString &source, const QString &path, QString *error = nullptr);

// First-contact dates, keyed by fingerprint.
QHash<QString, QDateTime> firstSeen();
void recordFirstSeen(const QString &host, const QString &fingerprint);
void forgetFirstSeen(const QStringList &fingerprints);

} // namespace knownhosts
