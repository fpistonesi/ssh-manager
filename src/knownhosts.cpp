#include "knownhosts.h"

#include "paths.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageAuthenticationCode>
#include <QRegularExpression>
#include <QSaveFile>

namespace knownhosts {

namespace {

QString metadataFile()
{
    return paths::configDir() + QStringLiteral("/known_hosts.meta.json");
}

// Lines of the file, without their line terminators.
bool readLines(const QString &path, QList<QByteArray> *lines, QString *error)
{
    QFile file(path);
    if (!file.exists())
        return true;
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    const QByteArray data = file.readAll();
    *lines = data.split('\n');
    if (!lines->isEmpty() && lines->last().isEmpty())
        lines->removeLast();
    for (QByteArray &line : *lines) {
        if (line.endsWith('\r'))
            line.chop(1);
    }
    return true;
}

bool writeLines(const QString &path, const QList<QByteArray> &lines, QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    for (const QByteArray &line : lines)
        file.write(line + '\n');
    if (!file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

bool parseLine(const QByteArray &raw, int number, Entry *entry)
{
    const QString line = QString::fromUtf8(raw).trimmed();
    if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
        return false;
    QStringList fields = line.split(QRegularExpression(QStringLiteral("\\s+")));
    Entry parsed;
    parsed.line = number;
    if (!fields.isEmpty() && fields.first().startsWith(QLatin1Char('@')))
        parsed.marker = fields.takeFirst();
    if (fields.size() < 3)
        return false;
    parsed.hosts = fields.at(0).split(QLatin1Char(','), Qt::SkipEmptyParts);
    parsed.hashed = fields.at(0).startsWith(QLatin1String("|1|"));
    parsed.keyType = fields.at(1);
    parsed.key = QByteArray::fromBase64(fields.at(2).toLatin1());
    if (parsed.key.isEmpty())
        return false;
    parsed.comment = fields.mid(3).join(QLatin1Char(' '));
    parsed.fingerprint = fingerprint(parsed.key);
    *entry = parsed;
    return true;
}

} // namespace

QVector<Entry> read(const QString &path, QString *error)
{
    QVector<Entry> entries;
    QList<QByteArray> lines;
    if (!readLines(path, &lines, error))
        return entries;
    for (int i = 0; i < lines.size(); ++i) {
        Entry entry;
        if (parseLine(lines.at(i), i, &entry))
            entries.append(entry);
    }
    return entries;
}

QString fingerprint(const QByteArray &keyBlob)
{
    QByteArray digest = QCryptographicHash::hash(keyBlob, QCryptographicHash::Sha256).toBase64();
    while (digest.endsWith('='))
        digest.chop(1);
    return QStringLiteral("SHA256:") + QString::fromLatin1(digest);
}

QString hostName(const QString &host, quint16 port)
{
    return port == 22 ? host : QStringLiteral("[%1]:%2").arg(host).arg(port);
}

bool matches(const Entry &entry, const QString &name)
{
    for (const QString &pattern : entry.hosts) {
        if (pattern.startsWith(QLatin1String("|1|"))) {
            // |1|base64(salt)|base64(HMAC-SHA1(salt, name))
            const QStringList parts = pattern.split(QLatin1Char('|'));
            if (parts.size() != 4)
                continue;
            const QByteArray salt = QByteArray::fromBase64(parts.at(2).toLatin1());
            const QByteArray hash = QByteArray::fromBase64(parts.at(3).toLatin1());
            if (QMessageAuthenticationCode::hash(name.toUtf8(), salt, QCryptographicHash::Sha1)
                == hash)
                return true;
        } else if (pattern.compare(name, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

bool removeLines(const QString &path, const QSet<int> &lines, QString *error)
{
    QList<QByteArray> all;
    if (!readLines(path, &all, error))
        return false;
    QList<QByteArray> kept;
    for (int i = 0; i < all.size(); ++i) {
        if (!lines.contains(i))
            kept.append(all.at(i));
    }
    return writeLines(path, kept, error);
}

int removeHost(const QString &path, const QString &name, QString *error)
{
    QSet<int> lines;
    QStringList fingerprints;
    for (const Entry &entry : read(path, error)) {
        if (entry.marker.isEmpty() && matches(entry, name)) {
            lines.insert(entry.line);
            fingerprints << entry.fingerprint;
        }
    }
    if (lines.isEmpty())
        return 0;
    if (!removeLines(path, lines, error))
        return -1;
    forgetFirstSeen(fingerprints);
    return int(lines.size());
}

int import(const QString &source, const QString &path, QString *error)
{
    QList<QByteArray> sourceLines;
    if (!readLines(source, &sourceLines, error))
        return -1;
    QList<QByteArray> lines;
    if (!readLines(path, &lines, error))
        return -1;

    // Same host field and same key: already there.
    QSet<QString> present;
    for (const Entry &entry : read(path))
        present.insert(entry.hosts.join(QLatin1Char(',')) + QLatin1Char(' ') + entry.fingerprint);

    int added = 0;
    for (int i = 0; i < sourceLines.size(); ++i) {
        Entry entry;
        if (!parseLine(sourceLines.at(i), i, &entry))
            continue;
        const QString id = entry.hosts.join(QLatin1Char(',')) + QLatin1Char(' ') + entry.fingerprint;
        if (present.contains(id))
            continue;
        present.insert(id);
        lines.append(sourceLines.at(i).trimmed());
        ++added;
    }
    if (added > 0 && !writeLines(path, lines, error))
        return -1;
    return added;
}

QHash<QString, QDateTime> firstSeen()
{
    QHash<QString, QDateTime> dates;
    QFile file(metadataFile());
    if (!file.open(QIODevice::ReadOnly))
        return dates;
    const QJsonObject keys = QJsonDocument::fromJson(file.readAll()).object();
    for (auto it = keys.begin(); it != keys.end(); ++it) {
        const QDateTime added = QDateTime::fromString(
            it.value().toObject().value(QStringLiteral("added")).toString(), Qt::ISODate);
        if (added.isValid())
            dates.insert(it.key(), added);
    }
    return dates;
}

namespace {

QJsonObject readMetadata()
{
    QFile file(metadataFile());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

void writeMetadata(const QJsonObject &keys)
{
    QSaveFile file(metadataFile());
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(keys).toJson(QJsonDocument::Indented));
    file.commit();
}

} // namespace

void recordFirstSeen(const QString &host, const QString &fingerprint)
{
    if (fingerprint.isEmpty())
        return;
    QJsonObject keys = readMetadata();
    if (keys.contains(fingerprint))
        return;
    QJsonObject entry;
    entry[QStringLiteral("host")] = host;
    entry[QStringLiteral("added")] = QDateTime::currentDateTime().toString(Qt::ISODate);
    keys[fingerprint] = entry;
    writeMetadata(keys);
}

void forgetFirstSeen(const QStringList &fingerprints)
{
    // A key may also be listed under other names: keep its date then.
    QSet<QString> stillUsed;
    for (const Entry &entry : read(paths::knownHostsFile()))
        stillUsed.insert(entry.fingerprint);

    QJsonObject keys = readMetadata();
    bool changed = false;
    for (const QString &fingerprint : fingerprints) {
        if (!stillUsed.contains(fingerprint) && keys.contains(fingerprint)) {
            keys.remove(fingerprint);
            changed = true;
        }
    }
    if (changed)
        writeMetadata(keys);
}

} // namespace knownhosts
