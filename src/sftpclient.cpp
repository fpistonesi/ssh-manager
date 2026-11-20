#include "sftpclient.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QProcess>
#include <QProcessEnvironment>

#include <algorithm>

namespace {

constexpr int kIoTimeoutMs = 30000;
constexpr quint32 kMaxPacket = 16 * 1024 * 1024;
constexpr int kChunkSize = 64 * 1024;
// READ/WRITE requests kept in flight per file: without pipelining every
// 64 KB chunk would cost a full round trip.
constexpr int kWindow = 16;

// SFTP v3 packet types.
enum PacketType : quint8 {
    FXP_INIT = 1,
    FXP_VERSION = 2,
    FXP_OPEN = 3,
    FXP_CLOSE = 4,
    FXP_READ = 5,
    FXP_WRITE = 6,
    FXP_SETSTAT = 9,
    FXP_OPENDIR = 11,
    FXP_READDIR = 12,
    FXP_REMOVE = 13,
    FXP_MKDIR = 14,
    FXP_RMDIR = 15,
    FXP_REALPATH = 16,
    FXP_STAT = 17,
    FXP_RENAME = 18,
    FXP_STATUS = 101,
    FXP_HANDLE = 102,
    FXP_DATA = 103,
    FXP_NAME = 104,
    FXP_ATTRS = 105
};

enum StatusCode : quint32 {
    FX_OK = 0,
    FX_EOF = 1,
    FX_NO_SUCH_FILE = 2,
    FX_PERMISSION_DENIED = 3,
    FX_FAILURE = 4,
    FX_NO_CONNECTION = 6,
    FX_CONNECTION_LOST = 7,
    FX_OP_UNSUPPORTED = 8
};

enum OpenFlag : quint32 {
    FXF_READ = 0x01,
    FXF_WRITE = 0x02,
    FXF_CREAT = 0x08,
    FXF_TRUNC = 0x10
};

enum AttrFlag : quint32 {
    ATTR_SIZE = 0x1,
    ATTR_UIDGID = 0x2,
    ATTR_PERMISSIONS = 0x4,
    ATTR_ACMODTIME = 0x8,
    ATTR_EXTENDED = 0x80000000
};

struct Attrs {
    quint64 size = 0;
    quint32 perms = 0;
    quint32 mtime = 0;
    bool hasTime = false;
};

void putU32(QByteArray &out, quint32 value)
{
    out.append(char((value >> 24) & 0xff));
    out.append(char((value >> 16) & 0xff));
    out.append(char((value >> 8) & 0xff));
    out.append(char(value & 0xff));
}

void putU64(QByteArray &out, quint64 value)
{
    for (int shift = 56; shift >= 0; shift -= 8)
        out.append(char((value >> shift) & 0xff));
}

void putString(QByteArray &out, const QByteArray &value)
{
    putU32(out, quint32(value.size()));
    out.append(value);
}

quint32 readU32At(const QByteArray &data, int offset)
{
    return (quint32(quint8(data.at(offset))) << 24) | (quint32(quint8(data.at(offset + 1))) << 16)
           | (quint32(quint8(data.at(offset + 2))) << 8) | quint32(quint8(data.at(offset + 3)));
}

class PacketReader {
public:
    explicit PacketReader(const QByteArray &data) : data_(data) {}

    quint32 u32()
    {
        if (pos_ + 4 > data_.size()) {
            ok_ = false;
            return 0;
        }
        const quint32 value = readU32At(data_, pos_);
        pos_ += 4;
        return value;
    }

    quint64 u64()
    {
        if (pos_ + 8 > data_.size()) {
            ok_ = false;
            return 0;
        }
        quint64 value = 0;
        for (int i = 0; i < 8; ++i)
            value = (value << 8) | quint8(data_.at(pos_ + i));
        pos_ += 8;
        return value;
    }

    QByteArray string()
    {
        const quint32 length = u32();
        if (!ok_ || length > quint32(data_.size() - pos_)) {
            ok_ = false;
            return QByteArray();
        }
        const QByteArray value = data_.mid(pos_, int(length));
        pos_ += int(length);
        return value;
    }

    bool ok() const { return ok_; }

private:
    const QByteArray &data_;
    int pos_ = 0;
    bool ok_ = true;
};

Attrs readAttrs(PacketReader &reader)
{
    Attrs attrs;
    const quint32 flags = reader.u32();
    if (flags & ATTR_SIZE) {
        attrs.size = reader.u64();
    }
    if (flags & ATTR_UIDGID) {
        reader.u32();
        reader.u32();
    }
    if (flags & ATTR_PERMISSIONS)
        attrs.perms = reader.u32();
    if (flags & ATTR_ACMODTIME) {
        reader.u32();
        attrs.mtime = reader.u32();
        attrs.hasTime = true;
    }
    if (flags & ATTR_EXTENDED) {
        const quint32 count = reader.u32();
        for (quint32 i = 0; i < count && reader.ok(); ++i) {
            reader.string();
            reader.string();
        }
    }
    return attrs;
}

struct SftpStatus {
    quint32 code = 0;
    QString message;
};

SftpStatus parseStatus(const QByteArray &body)
{
    SftpStatus status;
    PacketReader reader(body);
    reader.u32();  // request id
    status.code = reader.u32();
    status.message = QString::fromUtf8(reader.string());
    if (!reader.ok())
        status.code = FX_FAILURE;
    return status;
}

QString statusText(quint32 code, const QString &serverText = QString())
{
    switch (code) {
    case FX_EOF:
        return SftpSession::tr("Fine del file.");
    case FX_NO_SUCH_FILE:
        return SftpSession::tr("File o cartella non trovati.");
    case FX_PERMISSION_DENIED:
        return SftpSession::tr("Permesso negato dal server.");
    case FX_NO_CONNECTION:
    case FX_CONNECTION_LOST:
        return SftpSession::tr("Connessione persa.");
    case FX_OP_UNSUPPORTED:
        return SftpSession::tr("Operazione non supportata dal server SFTP.");
    case FX_FAILURE:
        break;
    default:
        break;
    }
    if (!serverText.trimmed().isEmpty())
        return serverText;
    return SftpSession::tr("Operazione rifiutata dal server SFTP (codice %1).").arg(code);
}

QString joinRemote(const QString &dir, const QString &name)
{
    if (dir.isEmpty())
        return name;
    if (dir.endsWith(QLatin1Char('/')))
        return dir + name;
    return dir + QLatin1Char('/') + name;
}

QString remoteBaseName(const QString &path)
{
    QString trimmed = path;
    while (trimmed.size() > 1 && trimmed.endsWith(QLatin1Char('/')))
        trimmed.chop(1);
    const int slash = trimmed.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? trimmed : trimmed.mid(slash + 1);
}

QString remoteParent(const QString &path)
{
    QString trimmed = path;
    while (trimmed.size() > 1 && trimmed.endsWith(QLatin1Char('/')))
        trimmed.chop(1);
    const int slash = trimmed.lastIndexOf(QLatin1Char('/'));
    return slash <= 0 ? QStringLiteral("/") : trimmed.left(slash);
}

bool isDirectoryMode(quint32 mode)
{
    return (mode & 0170000) == 0040000;
}

bool isLinkMode(quint32 mode)
{
    return (mode & 0170000) == 0120000;
}

} // namespace

SftpSession::SftpSession(QObject *parent) : QObject(parent) {}

SftpSession::~SftpSession()
{
    stopProcess();
    cleanupFiles();
}

void SftpSession::configure(const SftpPlan &plan)
{
    plan_ = plan;
}

void SftpSession::requestCancel()
{
    cancel_ = true;
}

bool SftpSession::startProcess(QString *error)
{
    if (!QFileInfo(plan_.program).isExecutable()) {
        *error = tr("Il client \"ssh\" non è stato trovato in %1.").arg(plan_.program);
        return false;
    }

    process_ = new QProcess(this);
    process_->setProgram(plan_.program);
    process_->setArguments(plan_.args);
    process_->setProcessChannelMode(QProcess::SeparateChannels);

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const QString &entry : plan_.environment) {
        const int equals = entry.indexOf(QLatin1Char('='));
        if (equals > 0)
            environment.insert(entry.left(equals), entry.mid(equals + 1));
    }
    process_->setProcessEnvironment(environment);

    process_->start();
    if (!process_->waitForStarted(15000)) {
        *error = tr("Impossibile avviare %1.").arg(plan_.program);
        stopProcess();
        return false;
    }
    return true;
}

void SftpSession::stopProcess()
{
    opened_ = false;
    if (!process_)
        return;
    if (process_->state() != QProcess::NotRunning) {
#ifdef _WIN32
        // terminate() only posts WM_CLOSE, which console programs like ssh.exe
        // ignore: close stdin instead (ssh ends the SFTP channel on EOF).
        process_->closeWriteChannel();
#else
        process_->terminate();
#endif
        if (!process_->waitForFinished(1000))
            process_->kill();
        process_->waitForFinished(1000);
    }
    process_->deleteLater();
    process_ = nullptr;
    buffer_.clear();
    stderrTail_.clear();
}

void SftpSession::cleanupFiles()
{
    if (ownsFiles_) {
        for (const QString &path : plan_.temporaryFiles)
            QFile::remove(path);
    }
    plan_.temporaryFiles.clear();
    plan_.environment.clear();
}

bool SftpSession::ensureOpen(QString *error)
{
    if (opened_ && process_ && process_->state() == QProcess::Running)
        return true;
    stopProcess();
    if (!startProcess(error) || !handshake(error)) {
        stopProcess();
        connectionError_ = true;
        return false;
    }
    QString home;
    if (!realPath(QStringLiteral("."), &home, error)) {
        stopProcess();
        return false;
    }
    home_ = home;
    opened_ = true;
    return true;
}

bool SftpSession::sendPacket(quint8 type, const QByteArray &payload, QString *error)
{
    if (!process_ || process_->state() != QProcess::Running) {
        *error = tr("La connessione SFTP non è attiva.");
        connectionError_ = true;
        return false;
    }

    QByteArray packet;
    putU32(packet, quint32(payload.size() + 1));
    packet.append(char(type));
    packet.append(payload);
    if (process_->write(packet) != packet.size()) {
        *error = tr("Invio al server SFTP non riuscito.");
        connectionError_ = true;
        return false;
    }
    return true;
}

bool SftpSession::receivePacket(quint8 *type, QByteArray *payload, QString *error)
{
    if (!waitForBytes(4, error))
        return false;

    const quint32 length = readU32At(buffer_, 0);
    if (length == 0 || length > kMaxPacket) {
        *error = tr("Risposta SFTP non valida.");
        return false;
    }
    if (!waitForBytes(int(length) + 4, error))
        return false;

    *type = quint8(buffer_.at(4));
    *payload = buffer_.mid(5, int(length) - 1);
    buffer_.remove(0, int(length) + 4);
    return true;
}

bool SftpSession::waitForBytes(int count, QString *error)
{
    QElapsedTimer timer;
    timer.start();

    while (buffer_.size() < count) {
        if (cancel_) {
            *error = tr("Operazione annullata.");
            return false;
        }
        if (!process_ || process_->state() == QProcess::NotRunning) {
            if (process_) {
                stderrTail_ += QString::fromUtf8(process_->readAllStandardError());
                if (stderrTail_.size() > 4000)
                    stderrTail_ = stderrTail_.right(4000);
            }
            *error = tr("Il server SFTP ha chiuso la connessione.");
            const QString diagnostics = stderrTail_.trimmed();
            if (!diagnostics.isEmpty())
                *error += QLatin1Char('\n') + diagnostics;
            connectionError_ = true;
            return false;
        }

        if (!process_->waitForReadyRead(200)) {
            const QByteArray stderrData = process_->readAllStandardError();
            if (!stderrData.isEmpty()) {
                stderrTail_ += QString::fromUtf8(stderrData);
                if (stderrTail_.size() > 4000)
                    stderrTail_ = stderrTail_.right(4000);
            }
            if (timer.elapsed() > kIoTimeoutMs) {
                *error = tr("Il server SFTP non risponde.");
                const QString diagnostics = stderrTail_.trimmed();
                if (!diagnostics.isEmpty())
                    *error += QLatin1Char('\n') + diagnostics;
                connectionError_ = true;
                return false;
            }
            continue;
        }

        buffer_.append(process_->readAllStandardOutput());
        const QByteArray stderrData = process_->readAllStandardError();
        if (!stderrData.isEmpty()) {
            stderrTail_ += QString::fromUtf8(stderrData);
            if (stderrTail_.size() > 4000)
                stderrTail_ = stderrTail_.right(4000);
        }
    }
    return true;
}

bool SftpSession::sendRequest(quint8 type, const QByteArray &body, quint32 *id, QString *error)
{
    *id = ++lastId_;
    QByteArray payload;
    putU32(payload, *id);
    payload.append(body);
    return sendPacket(type, payload, error);
}

bool SftpSession::receiveResponse(quint32 *id, quint8 *type, QByteArray *payload, QString *error)
{
    for (;;) {
        if (!receivePacket(type, payload, error))
            return false;
        if (*type == FXP_VERSION)
            continue;
        PacketReader reader(*payload);
        *id = reader.u32();
        if (!reader.ok()) {
            *error = tr("Risposta SFTP non valida.");
            return false;
        }
        return true;
    }
}

bool SftpSession::call(quint8 type, const QByteArray &body, quint8 *responseType,
                       QByteArray *response, QString *error)
{
    quint32 id = 0;
    if (!sendRequest(type, body, &id, error))
        return false;
    for (;;) {
        quint32 incomingId = 0;
        if (!receiveResponse(&incomingId, responseType, response, error))
            return false;
        if (incomingId == id)
            return true;
    }
}

bool SftpSession::statusCall(quint8 type, const QByteArray &body, QString *error)
{
    quint8 responseType = 0;
    QByteArray response;
    if (!call(type, body, &responseType, &response, error))
        return false;
    if (responseType != FXP_STATUS) {
        *error = tr("Risposta SFTP non valida.");
        return false;
    }
    const SftpStatus status = parseStatus(response);
    if (status.code != FX_OK) {
        *error = statusText(status.code, status.message);
        return false;
    }
    return true;
}

bool SftpSession::handshake(QString *error)
{
    QByteArray version;
    putU32(version, 3);
    if (!sendPacket(FXP_INIT, version, error))
        return false;

    quint8 type = 0;
    QByteArray payload;
    if (!receivePacket(&type, &payload, error))
        return false;
    if (type != FXP_VERSION) {
        *error = tr("Il server non ha risposto al protocollo SFTP.");
        return false;
    }

    PacketReader reader(payload);
    const quint32 serverVersion = reader.u32();
    if (!reader.ok() || serverVersion < 3) {
        *error = tr("Versione SFTP non supportata dal server (%1).").arg(serverVersion);
        return false;
    }
    return true;
}

bool SftpSession::realPath(const QString &path, QString *out, QString *error)
{
    QByteArray body;
    putString(body, path.toUtf8());

    quint8 type = 0;
    QByteArray response;
    if (!call(FXP_REALPATH, body, &type, &response, error))
        return false;
    if (type != FXP_NAME) {
        const SftpStatus status = parseStatus(response);
        *error = statusText(status.code, status.message);
        return false;
    }

    PacketReader reader(response);
    reader.u32();
    const quint32 count = reader.u32();
    const QByteArray name = reader.string();
    if (!reader.ok() || count == 0) {
        *error = tr("Percorso remoto non valido.");
        return false;
    }
    *out = QString::fromUtf8(name);
    return true;
}

bool SftpSession::statEntry(const QString &path, RemoteEntry *entry, QString *error)
{
    QByteArray body;
    putString(body, path.toUtf8());

    quint8 type = 0;
    QByteArray response;
    if (!call(FXP_STAT, body, &type, &response, error))
        return false;
    if (type != FXP_ATTRS) {
        const SftpStatus status = parseStatus(response);
        *error = statusText(status.code, status.message);
        return false;
    }

    PacketReader reader(response);
    reader.u32();
    const Attrs attrs = readAttrs(reader);
    if (!reader.ok()) {
        *error = tr("Risposta SFTP non valida.");
        return false;
    }

    entry->path = path;
    entry->name = remoteBaseName(path);
    entry->mode = attrs.perms;
    entry->isDir = isDirectoryMode(attrs.perms);
    entry->isLink = isLinkMode(attrs.perms);
    entry->size = qint64(attrs.size);
    if (attrs.hasTime)
        entry->modified = QDateTime::fromSecsSinceEpoch(qint64(attrs.mtime));
    return true;
}

QVector<RemoteEntry> SftpSession::readDirectory(const QString &path, QString *error)
{
    QVector<RemoteEntry> entries;

    QByteArray body;
    putString(body, path.toUtf8());
    quint8 type = 0;
    QByteArray response;
    if (!call(FXP_OPENDIR, body, &type, &response, error))
        return entries;
    if (type != FXP_HANDLE) {
        const SftpStatus status = parseStatus(response);
        *error = statusText(status.code, status.message);
        return entries;
    }

    PacketReader handleReader(response);
    handleReader.u32();
    const QByteArray handle = handleReader.string();
    if (!handleReader.ok()) {
        *error = tr("Risposta SFTP non valida.");
        return entries;
    }

    for (;;) {
        QByteArray listBody;
        putString(listBody, handle);
        quint8 listType = 0;
        QByteArray listResponse;
        if (!call(FXP_READDIR, listBody, &listType, &listResponse, error))
            break;

        if (listType == FXP_STATUS) {
            const SftpStatus status = parseStatus(listResponse);
            if (status.code != FX_EOF && status.code != FX_OK)
                *error = statusText(status.code, status.message);
            break;
        }
        if (listType != FXP_NAME) {
            *error = tr("Risposta SFTP non valida.");
            break;
        }

        PacketReader reader(listResponse);
        reader.u32();
        const quint32 count = reader.u32();
        for (quint32 i = 0; i < count && reader.ok(); ++i) {
            const QString name = QString::fromUtf8(reader.string());
            reader.string();  // long name
            const Attrs attrs = readAttrs(reader);
            if (!reader.ok())
                break;
            if (name == QLatin1String(".") || name == QLatin1String(".."))
                continue;

            RemoteEntry entry;
            entry.name = name;
            entry.path = joinRemote(path, name);
            entry.mode = attrs.perms;
            entry.isDir = isDirectoryMode(attrs.perms);
            entry.isLink = isLinkMode(attrs.perms);
            entry.size = qint64(attrs.size);
            if (attrs.hasTime)
                entry.modified = QDateTime::fromSecsSinceEpoch(qint64(attrs.mtime));

            if (entry.isLink) {
                // Resolve the target to know whether the entry can be entered.
                RemoteEntry resolved;
                QString ignored;
                if (statEntry(entry.path, &resolved, &ignored)) {
                    entry.isDir = resolved.isDir;
                    entry.size = resolved.size;
                }
            }
            entries.append(entry);
        }
        if (!reader.ok()) {
            *error = tr("Risposta SFTP non valida.");
            break;
        }
    }

    QString ignored;
    closeFile(handle, &ignored);

    std::sort(entries.begin(), entries.end(), [](const RemoteEntry &a, const RemoteEntry &b) {
        if (a.isDir != b.isDir)
            return a.isDir;
        return QString::localeAwareCompare(a.name, b.name) < 0;
    });
    return entries;
}

bool SftpSession::ensureDirectory(const QString &path, QString *error)
{
    QByteArray body;
    putString(body, path.toUtf8());
    putU32(body, ATTR_PERMISSIONS);
    putU32(body, 0755);

    quint8 type = 0;
    QByteArray response;
    if (!call(FXP_MKDIR, body, &type, &response, error))
        return false;
    if (type == FXP_STATUS && parseStatus(response).code == FX_OK)
        return true;

    RemoteEntry entry;
    QString ignored;
    if (statEntry(path, &entry, &ignored) && entry.isDir)
        return true;

    *error = tr("Impossibile creare la cartella remota %1.").arg(path);
    return false;
}

bool SftpSession::removeRemote(const QString &path)
{
    QByteArray body;
    putString(body, path.toUtf8());
    quint8 type = 0;
    QByteArray response;
    QString ignored;
    if (!call(FXP_REMOVE, body, &type, &response, &ignored))
        return false;
    return type == FXP_STATUS && parseStatus(response).code == FX_OK;
}

bool SftpSession::openFile(const QString &path, quint32 flags, quint32 mode, QByteArray *handle,
                           QString *error)
{
    QByteArray body;
    putString(body, path.toUtf8());
    putU32(body, flags);
    putU32(body, ATTR_PERMISSIONS);
    putU32(body, mode);

    quint8 type = 0;
    QByteArray response;
    if (!call(FXP_OPEN, body, &type, &response, error))
        return false;
    if (type != FXP_HANDLE) {
        const SftpStatus status = parseStatus(response);
        *error = statusText(status.code, status.message);
        return false;
    }

    PacketReader reader(response);
    reader.u32();
    *handle = reader.string();
    if (!reader.ok()) {
        *error = tr("Risposta SFTP non valida.");
        return false;
    }
    return true;
}

bool SftpSession::closeFile(const QByteArray &handle, QString *error)
{
    QByteArray body;
    putString(body, handle);

    quint8 type = 0;
    QByteArray response;
    if (!call(FXP_CLOSE, body, &type, &response, error))
        return false;
    if (type != FXP_STATUS) {
        *error = tr("Risposta SFTP non valida.");
        return false;
    }
    const SftpStatus status = parseStatus(response);
    if (status.code != FX_OK) {
        *error = statusText(status.code, status.message);
        return false;
    }
    return true;
}

bool SftpSession::scanLocal(const QString &localDir, const QString &remoteDir, int rootIndex,
                            QVector<TransferItem> *items, qint64 *total, QString *error)
{
    TransferItem directory;
    directory.rootIndex = rootIndex;
    directory.upload = true;
    directory.directory = true;
    directory.source = localDir;
    directory.target = remoteDir;
    items->append(directory);

    const QFileInfoList entries =
        QDir(localDir).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden
                                        | QDir::System,
                                    QDir::Name);
    for (const QFileInfo &entry : entries) {
        // Symlinks are skipped: they could duplicate whole trees or loop.
        if (entry.isSymLink())
            continue;
        if (entry.isDir()) {
            if (!scanLocal(entry.absoluteFilePath(), joinRemote(remoteDir, entry.fileName()),
                           rootIndex, items, total, error))
                return false;
            continue;
        }
        if (!entry.isFile())
            continue;
        TransferItem item;
        item.rootIndex = rootIndex;
        item.upload = true;
        item.source = entry.absoluteFilePath();
        item.target = joinRemote(remoteDir, entry.fileName());
        item.size = entry.size();
        items->append(item);
        *total += item.size;
    }
    return true;
}

bool SftpSession::buildUploadPlan(const QStringList &sources, const QString &destination,
                                  QVector<TransferItem> *items, qint64 *total, QString *error)
{
    for (int i = 0; i < sources.size(); ++i) {
        const QFileInfo info(sources.at(i));
        if (!info.exists()) {
            *error = tr("%1 non esiste più.").arg(sources.at(i));
            return false;
        }
        const QString target = joinRemote(destination, info.fileName());
        if (info.isDir()) {
            if (!scanLocal(info.absoluteFilePath(), target, i, items, total, error))
                return false;
        } else {
            TransferItem item;
            item.rootIndex = i;
            item.upload = true;
            item.source = info.absoluteFilePath();
            item.target = target;
            item.size = info.size();
            items->append(item);
            *total += item.size;
        }
    }
    return true;
}

bool SftpSession::scanRemote(const QString &remoteDir, const QString &localDir, int rootIndex,
                             QVector<TransferItem> *items, qint64 *total, QString *error)
{
    TransferItem directory;
    directory.rootIndex = rootIndex;
    directory.upload = false;
    directory.directory = true;
    directory.source = remoteDir;
    directory.target = localDir;
    items->append(directory);

    error->clear();
    const QVector<RemoteEntry> entries = readDirectory(remoteDir, error);
    if (!error->isEmpty())
        return false;

    for (const RemoteEntry &entry : entries) {
        const QString local = QDir(localDir).filePath(entry.name);
        if (entry.isDir) {
            // Never follow directory symlinks: they can loop.
            if (entry.isLink)
                continue;
            if (!scanRemote(entry.path, local, rootIndex, items, total, error))
                return false;
            continue;
        }
        TransferItem item;
        item.rootIndex = rootIndex;
        item.upload = false;
        item.source = entry.path;
        item.target = local;
        item.size = entry.size;
        items->append(item);
        *total += item.size;
    }
    return true;
}

bool SftpSession::buildDownloadPlan(const QStringList &sources, const QString &destination,
                                    QVector<TransferItem> *items, qint64 *total, QString *error)
{
    for (int i = 0; i < sources.size(); ++i) {
        const QString source = sources.at(i);
        const QString name = remoteBaseName(source);
        if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")) {
            *error = tr("Percorso remoto non valido: %1.").arg(source);
            return false;
        }
        const QString target = QDir(destination).filePath(name);

        RemoteEntry entry;
        if (!statEntry(source, &entry, error))
            return false;
        if (entry.isDir) {
            if (!scanRemote(source, target, i, items, total, error))
                return false;
        } else {
            TransferItem item;
            item.rootIndex = i;
            item.upload = false;
            item.source = source;
            item.target = target;
            item.size = entry.size;
            items->append(item);
            *total += item.size;
        }
    }
    return true;
}

void SftpSession::emitProgress(int jobId, qint64 done, qint64 total, const QString &current)
{
    if (progressTimer_.isValid() && progressTimer_.elapsed() < 100 && done < total)
        return;
    progressTimer_.restart();
    emit jobProgress(jobId, qBound<qint64>(0, done, qMax<qint64>(0, total)), total, current);
}

bool SftpSession::uploadFile(const TransferItem &item, bool resume, int jobId, qint64 *done,
                             qint64 total, QString *error)
{
    QFile file(item.source);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = tr("Impossibile leggere %1.").arg(item.source);
        return false;
    }

    // Resume: continue after what is already on the server (a retry of this
    // same transfer wrote it); a complete file is skipped.
    qint64 offset = 0;
    if (resume) {
        RemoteEntry existing;
        QString ignored;
        if (statEntry(item.target, &existing, &ignored) && !existing.isDir) {
            if (existing.size == file.size()) {
                *done += file.size();
                return true;
            }
            if (existing.size < file.size())
                offset = existing.size;
        }
    }
    if (offset > 0 && !file.seek(offset))
        offset = 0;
    *done += offset;

    QByteArray handle;
    const quint32 flags = offset > 0 ? (FXF_WRITE | FXF_CREAT) : (FXF_WRITE | FXF_CREAT | FXF_TRUNC);
    if (!openFile(item.target, flags, 0644, &handle, error))
        return false;

    const QString name = QFileInfo(item.source).fileName();
    QHash<quint32, int> inFlight; // request id -> bytes
    bool ok = true;
    bool endOfFile = false;
    while (ok) {
        if (cancel_) {
            *error = tr("Trasferimento annullato.");
            ok = false;
            break;
        }
        // Keep the window full.
        while (!endOfFile && inFlight.size() < kWindow) {
            const QByteArray chunk = file.read(kChunkSize);
            if (chunk.isEmpty()) {
                endOfFile = true;
                break;
            }
            QByteArray body;
            putString(body, handle);
            putU64(body, quint64(offset));
            putString(body, chunk);
            quint32 id = 0;
            if (!sendRequest(FXP_WRITE, body, &id, error)) {
                ok = false;
                break;
            }
            inFlight.insert(id, int(chunk.size()));
            offset += chunk.size();
        }
        if (!ok || inFlight.isEmpty())
            break;

        quint32 id = 0;
        quint8 type = 0;
        QByteArray response;
        if (!receiveResponse(&id, &type, &response, error)) {
            ok = false;
            break;
        }
        if (!inFlight.contains(id))
            continue;
        const SftpStatus status = type == FXP_STATUS ? parseStatus(response) : SftpStatus{FX_FAILURE, {}};
        if (status.code != FX_OK) {
            *error = statusText(status.code, status.message);
            ok = false;
            break;
        }
        *done += inFlight.take(id);
        emitProgress(jobId, *done, total, name);
    }

    if (connectionError_)
        return false; // the handle died with the connection
    // Drain the answers still due before closing the handle.
    while (!inFlight.isEmpty()) {
        quint32 id = 0;
        quint8 type = 0;
        QByteArray response;
        QString ignored;
        if (!receiveResponse(&id, &type, &response, &ignored))
            return false;
        inFlight.remove(id);
    }

    QString closeError;
    if (!closeFile(handle, &closeError) && ok) {
        *error = closeError;
        ok = false;
    }
    // A cancelled upload leaves nothing behind; a failed one is kept for
    // resuming.
    if (!ok && cancel_)
        removeRemote(item.target);
    return ok;
}

bool SftpSession::downloadFile(const TransferItem &item, bool resume, int jobId, qint64 *done,
                               qint64 total, QString *error)
{
    const QString partial = item.target + partialSuffix();

    // Resume: a complete target is skipped, a partial download continues.
    if (resume && QFileInfo(item.target).size() == item.size && QFileInfo::exists(item.target)
        && !QFileInfo::exists(partial)) {
        *done += item.size;
        return true;
    }
    QFile out(partial);
    qint64 offset = 0;
    if (resume && out.exists() && out.size() <= item.size) {
        offset = out.size();
        if (!out.open(QIODevice::WriteOnly | QIODevice::Append))
            offset = -1;
    } else {
        offset = out.open(QIODevice::WriteOnly | QIODevice::Truncate) ? 0 : -1;
    }
    if (offset < 0) {
        *error = tr("Impossibile scrivere %1.").arg(partial);
        return false;
    }
    *done += offset;

    QByteArray handle;
    if (!openFile(item.source, FXF_READ, 0, &handle, error))
        return false;

    const QString name = QFileInfo(item.target).fileName();
    struct Request {
        qint64 offset;
        int length;
    };
    QHash<quint32, Request> inFlight;
    QMap<qint64, QByteArray> pending; // data arrived ahead of the write position
    qint64 nextOffset = offset;       // next byte to request
    qint64 writeOffset = offset;      // next byte to write
    bool endOfFile = false;
    bool ok = true;

    const auto request = [&](qint64 at, int length) {
        QByteArray body;
        putString(body, handle);
        putU64(body, quint64(at));
        putU32(body, quint32(length));
        quint32 id = 0;
        if (!sendRequest(FXP_READ, body, &id, error))
            return false;
        inFlight.insert(id, Request{at, length});
        return true;
    };

    while (ok) {
        if (cancel_) {
            *error = tr("Trasferimento annullato.");
            ok = false;
            break;
        }
        while (!endOfFile && inFlight.size() < kWindow) {
            if (!request(nextOffset, kChunkSize)) {
                ok = false;
                break;
            }
            nextOffset += kChunkSize;
        }
        if (!ok || inFlight.isEmpty())
            break;

        quint32 id = 0;
        quint8 type = 0;
        QByteArray response;
        if (!receiveResponse(&id, &type, &response, error)) {
            ok = false;
            break;
        }
        if (!inFlight.contains(id))
            continue;
        const Request sent = inFlight.take(id);

        if (type == FXP_STATUS) {
            const SftpStatus status = parseStatus(response);
            if (status.code == FX_EOF) {
                endOfFile = true; // nothing at or after this offset
                continue;
            }
            *error = statusText(status.code, status.message);
            ok = false;
            break;
        }
        if (type != FXP_DATA) {
            *error = tr("Risposta SFTP non valida.");
            ok = false;
            break;
        }
        PacketReader reader(response);
        reader.u32();
        const QByteArray data = reader.string();
        if (!reader.ok()) {
            *error = tr("Risposta SFTP non valida.");
            ok = false;
            break;
        }
        // A short read is not the end of the file: ask for the rest.
        if (!data.isEmpty() && data.size() < sent.length
            && !request(sent.offset + data.size(), sent.length - int(data.size()))) {
            ok = false;
            break;
        }
        if (!data.isEmpty())
            pending.insert(sent.offset, data);

        while (pending.contains(writeOffset)) {
            const QByteArray chunk = pending.take(writeOffset);
            if (out.write(chunk) != chunk.size()) {
                *error = tr("Scrittura locale non riuscita per %1.").arg(partial);
                ok = false;
                break;
            }
            writeOffset += chunk.size();
            *done += chunk.size();
        }
        emitProgress(jobId, *done, total, name);
    }

    out.close();
    if (connectionError_)
        return false;
    while (!inFlight.isEmpty()) {
        quint32 id = 0;
        quint8 type = 0;
        QByteArray response;
        QString ignored;
        if (!receiveResponse(&id, &type, &response, &ignored))
            return false;
        inFlight.remove(id);
    }
    QString closeError;
    if (!closeFile(handle, &closeError) && ok) {
        *error = closeError;
        ok = false;
    }
    if (ok && !pending.isEmpty()) {
        *error = tr("Dati incompleti ricevuti per %1.").arg(name);
        ok = false;
    }

    if (ok) {
        QFile::remove(item.target);
        if (!QFile::rename(partial, item.target)) {
            *error = tr("Impossibile salvare %1.").arg(item.target);
            return false;
        }
    } else if (cancel_) {
        QFile::remove(partial); // a failed download stays for resuming
    }
    return ok;
}

void SftpSession::open()
{
    cancel_ = false;
    QString error;
    if (!ensureOpen(&error)) {
        emit failed(error);
        return;
    }
    emit opened(home_);
}

void SftpSession::listDirectory(const QString &path)
{
    cancel_ = false; // a cancel meant for an earlier operation does not apply
    QString error;
    if (!ensureOpen(&error)) {
        emit failed(error);
        return;
    }

    const QVector<RemoteEntry> entries = readDirectory(path, &error);
    if (!error.isEmpty()) {
        emit failed(error);
        return;
    }
    emit listed(path, entries);
}

void SftpSession::runJob(int jobId, bool upload, const QString &source,
                         const QString &destinationDir, bool resume)
{
    cancel_ = false;
    connectionError_ = false;
    progressTimer_.invalidate();

    QString error;
    const auto finish = [&](bool ok) {
        const bool cancelled = !ok && cancel_;
        if (cancelled || connectionError_) {
            // The protocol state is unknown after an interruption: start over
            // with a fresh connection for the next job.
            stopProcess();
        }
        emit jobFinished(jobId, ok, cancelled ? tr("Trasferimento annullato.") : error, cancelled,
                         !ok && !cancelled && connectionError_);
    };

    if (!ensureOpen(&error)) {
        finish(false);
        return;
    }

    QVector<TransferItem> items;
    qint64 total = 0;
    const bool planned =
        upload ? buildUploadPlan({source}, destinationDir, &items, &total, &error)
               : buildDownloadPlan({source}, destinationDir, &items, &total, &error);
    if (!planned) {
        finish(false);
        return;
    }
    int files = 0;
    for (const TransferItem &item : items)
        files += item.directory ? 0 : 1;
    emit jobPlanned(jobId, total, files);

    qint64 done = 0;
    for (const TransferItem &item : items) {
        if (cancel_) {
            finish(false);
            return;
        }
        bool ok = true;
        if (item.directory) {
            if (upload) {
                ok = ensureDirectory(item.target, &error);
            } else {
                ok = QDir().mkpath(item.target);
                if (!ok)
                    error = tr("Impossibile creare la cartella %1.").arg(item.target);
            }
        } else if (upload) {
            ok = uploadFile(item, resume, jobId, &done, total, &error);
        } else {
            ok = downloadFile(item, resume, jobId, &done, total, &error);
        }
        if (!ok) {
            finish(false);
            return;
        }
    }

    emit jobProgress(jobId, total, total, QString());
    finish(true);
}

void SftpSession::makeDirectory(const QString &path)
{
    cancel_ = false;
    QString error;
    bool ok = ensureOpen(&error);
    if (ok) {
        QByteArray body;
        putString(body, path.toUtf8());
        putU32(body, ATTR_PERMISSIONS);
        putU32(body, 0755);
        ok = statusCall(FXP_MKDIR, body, &error);
    }
    emit operationFinished(remoteParent(path), ok,
                           ok ? QString() : tr("Impossibile creare %1: %2").arg(path, error));
}

void SftpSession::renamePath(const QString &from, const QString &to)
{
    cancel_ = false;
    QString error;
    bool ok = ensureOpen(&error);
    if (ok) {
        QByteArray body;
        putString(body, from.toUtf8());
        putString(body, to.toUtf8());
        ok = statusCall(FXP_RENAME, body, &error);
    }
    emit operationFinished(remoteParent(to), ok,
                           ok ? QString() : tr("Impossibile rinominare %1: %2").arg(from, error));
}

bool SftpSession::removeTree(const QString &path, int kind, QString *error)
{
    // lstat semantics matter here: a symlink to a directory is removed as a
    // link, never followed. The type comes from the parent's listing, where
    // links are flagged.
    if (kind < 0) {
        QString listError;
        kind = 0;
        for (const RemoteEntry &entry : readDirectory(remoteParent(path), &listError)) {
            if (entry.name == remoteBaseName(path)) {
                kind = entry.isDir && !entry.isLink ? 1 : 0;
                break;
            }
        }
    }
    QByteArray body;
    putString(body, path.toUtf8());
    if (kind == 0)
        return statusCall(FXP_REMOVE, body, error);

    QString listError;
    const QVector<RemoteEntry> children = readDirectory(path, &listError);
    if (!listError.isEmpty()) {
        *error = listError;
        return false;
    }
    for (const RemoteEntry &child : children) {
        if (cancel_) {
            *error = tr("Operazione annullata.");
            return false;
        }
        if (!removeTree(child.path, child.isDir && !child.isLink ? 1 : 0, error)) {
            if (!error->startsWith(child.path))
                *error = tr("%1: %2").arg(child.path, *error);
            return false;
        }
    }
    return statusCall(FXP_RMDIR, body, error);
}

void SftpSession::removePaths(const QStringList &paths)
{
    cancel_ = false;
    QString error;
    bool ok = ensureOpen(&error);
    for (const QString &path : paths) {
        if (!ok)
            break;
        ok = removeTree(path, -1, &error);
        if (!ok)
            error = tr("Impossibile eliminare %1: %2").arg(path, error);
    }
    emit operationFinished(paths.isEmpty() ? QString() : remoteParent(paths.first()), ok,
                           ok ? QString() : error);
}

void SftpSession::setPermissions(const QStringList &paths, quint32 mode)
{
    cancel_ = false;
    QString error;
    bool ok = ensureOpen(&error);
    for (const QString &path : paths) {
        if (!ok)
            break;
        QByteArray body;
        putString(body, path.toUtf8());
        putU32(body, ATTR_PERMISSIONS);
        putU32(body, mode & 07777);
        ok = statusCall(FXP_SETSTAT, body, &error);
        if (!ok)
            error = tr("Impossibile cambiare i permessi di %1: %2").arg(path, error);
    }
    emit operationFinished(paths.isEmpty() ? QString() : remoteParent(paths.first()), ok,
                           ok ? QString() : error);
}

void SftpSession::fetchForEdit(const QString &remotePath, const QString &localPath)
{
    cancel_ = false;
    QString error;
    bool ok = ensureOpen(&error);
    RemoteEntry entry;
    if (ok)
        ok = statEntry(remotePath, &entry, &error);
    if (ok) {
        TransferItem item;
        item.upload = false;
        item.source = remotePath;
        item.target = localPath;
        item.size = entry.size;
        qint64 done = 0;
        ok = downloadFile(item, false, -1, &done, entry.size, &error);
    }
    emit editFetched(remotePath, localPath, ok, error);
}

void SftpSession::storeFromEdit(const QString &localPath, const QString &remotePath)
{
    cancel_ = false;
    QString error;
    bool ok = ensureOpen(&error);
    if (ok) {
        TransferItem item;
        item.upload = true;
        item.source = localPath;
        item.target = remotePath;
        item.size = QFileInfo(localPath).size();
        qint64 done = 0;
        ok = uploadFile(item, false, -1, &done, item.size, &error);
    }
    emit editStored(remotePath, ok, error);
}

void SftpSession::close()
{
    stopProcess();
    cleanupFiles();
}
