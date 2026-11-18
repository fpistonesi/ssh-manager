#include "model.h"

#include "crypto.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QUuid>

#include <functional>

namespace {

QString encodeBytes(const QByteArray &data)
{
    return QString::fromLatin1(data.toBase64());
}

QByteArray decodeBytes(const QString &text)
{
    return QByteArray::fromBase64(text.toLatin1());
}

QJsonObject tunnelToJson(const TunnelConfig &t)
{
    QJsonObject o;
    o[QStringLiteral("localAddress")] = t.localAddress;
    o[QStringLiteral("localPort")] = t.localPort;
    o[QStringLiteral("remoteHost")] = t.remoteHost;
    o[QStringLiteral("remotePort")] = t.remotePort;
    o[QStringLiteral("enabled")] = t.enabled;
    return o;
}

TunnelConfig tunnelFromJson(const QJsonObject &o)
{
    TunnelConfig t;
    t.localAddress = o.value(QStringLiteral("localAddress")).toString(QStringLiteral("127.0.0.1"));
    t.localPort = static_cast<quint16>(o.value(QStringLiteral("localPort")).toInt(8080));
    t.remoteHost = o.value(QStringLiteral("remoteHost")).toString(QStringLiteral("127.0.0.1"));
    t.remotePort = static_cast<quint16>(o.value(QStringLiteral("remotePort")).toInt(80));
    t.enabled = o.value(QStringLiteral("enabled")).toBool(true);
    return t;
}

QJsonObject terminalToJson(const TerminalProfile &t)
{
    QJsonObject o;
    o[QStringLiteral("colorScheme")] = t.colorScheme;
    o[QStringLiteral("fontFamily")] = t.fontFamily;
    o[QStringLiteral("fontSize")] = t.fontSize;
    o[QStringLiteral("cursorShape")] = t.cursorShape;
    o[QStringLiteral("historySize")] = t.historySize;
    o[QStringLiteral("opacityPercent")] = t.opacityPercent;
    o[QStringLiteral("margin")] = t.margin;
    o[QStringLiteral("blinkCursor")] = t.blinkCursor;
    return o;
}

TerminalProfile terminalFromJson(const QJsonObject &o)
{
    TerminalProfile t;
    t.colorScheme = o.value(QStringLiteral("colorScheme")).toString(t.colorScheme);
    t.fontFamily = o.value(QStringLiteral("fontFamily")).toString();
    t.fontSize = qBound(6, o.value(QStringLiteral("fontSize")).toInt(t.fontSize), 48);
    t.cursorShape = qBound(0, o.value(QStringLiteral("cursorShape")).toInt(t.cursorShape), 2);
    t.historySize =
        qBound(100, o.value(QStringLiteral("historySize")).toInt(t.historySize), 200000);
    t.opacityPercent =
        qBound(20, o.value(QStringLiteral("opacityPercent")).toInt(t.opacityPercent), 100);
    t.margin = qBound(0, o.value(QStringLiteral("margin")).toInt(t.margin), 24);
    t.blinkCursor = o.value(QStringLiteral("blinkCursor")).toBool(t.blinkCursor);
    return t;
}

QJsonObject connectionToJson(const Connection &c)
{
    QJsonObject o;
    o[QStringLiteral("id")] = c.id;
    o[QStringLiteral("name")] = c.name;
    o[QStringLiteral("host")] = c.host;
    o[QStringLiteral("port")] = c.port;
    o[QStringLiteral("user")] = c.user;
    o[QStringLiteral("auth")] = static_cast<int>(c.auth);
    o[QStringLiteral("keyPath")] = c.keyPath;
    o[QStringLiteral("keyName")] = c.keyName;
    o[QStringLiteral("jumpId")] = c.jumpId;
    o[QStringLiteral("notes")] = c.notes;
    o[QStringLiteral("favorite")] = c.favorite;
    QJsonArray tags;
    for (const QString &tag : c.tags)
        tags.append(tag);
    o[QStringLiteral("tags")] = tags;
    o[QStringLiteral("password")] = encodeBytes(c.password);
    o[QStringLiteral("passphrase")] = encodeBytes(c.passphrase);
    o[QStringLiteral("privateKey")] = encodeBytes(c.privateKey);

    QJsonArray tunnels;
    for (const TunnelConfig &t : c.tunnels)
        tunnels.append(tunnelToJson(t));
    o[QStringLiteral("tunnels")] = tunnels;
    o[QStringLiteral("terminal")] = terminalToJson(c.terminal);
    return o;
}

Connection connectionFromJson(const QJsonObject &o, bool legacy)
{
    Connection c;
    c.id = o.value(QStringLiteral("id")).toString();
    if (c.id.isEmpty())
        c.id = ConnectionStore::newId();
    c.name = o.value(QStringLiteral("name")).toString();
    c.host = o.value(QStringLiteral("host")).toString();
    c.port = static_cast<quint16>(o.value(QStringLiteral("port")).toInt(22));
    c.user = o.value(QStringLiteral("user")).toString();
    c.auth = static_cast<AuthType>(o.value(QStringLiteral("auth")).toInt(0));
    c.keyPath = o.value(QStringLiteral("keyPath")).toString();
    c.keyName = o.value(QStringLiteral("keyName")).toString();
    c.jumpId = o.value(QStringLiteral("jumpId")).toString();
    c.notes = o.value(QStringLiteral("notes")).toString();
    c.favorite = o.value(QStringLiteral("favorite")).toBool(false);
    for (const QJsonValue &tag : o.value(QStringLiteral("tags")).toArray()) {
        const QString value = tag.toString().trimmed();
        if (!value.isEmpty())
            c.tags.append(value);
    }

    if (legacy) {
        // Old format: each secret was encrypted separately with master.key.
        c.password = crypto::decryptString(o.value(QStringLiteral("password")).toString()).toUtf8();
        c.passphrase = crypto::decryptString(o.value(QStringLiteral("passphrase")).toString()).toUtf8();
    } else {
        c.password = decodeBytes(o.value(QStringLiteral("password")).toString());
        c.passphrase = decodeBytes(o.value(QStringLiteral("passphrase")).toString());
        c.privateKey = decodeBytes(o.value(QStringLiteral("privateKey")).toString());
    }

    for (const QJsonValue &v : o.value(QStringLiteral("tunnels")).toArray())
        c.tunnels.append(tunnelFromJson(v.toObject()));
    c.terminal = terminalFromJson(o.value(QStringLiteral("terminal")).toObject());
    return c;
}

QJsonObject nodeToJson(const Node &n)
{
    QJsonObject o;
    o[QStringLiteral("id")] = n.id;
    o[QStringLiteral("name")] = n.name;
    if (n.kind == Node::Kind::Folder) {
        o[QStringLiteral("type")] = QStringLiteral("folder");
        o[QStringLiteral("expanded")] = n.expanded;
        QJsonArray children;
        for (const Node &child : n.children)
            children.append(nodeToJson(child));
        o[QStringLiteral("children")] = children;
    } else {
        o[QStringLiteral("type")] = QStringLiteral("connection");
        o[QStringLiteral("connection")] = connectionToJson(n.conn);
    }
    return o;
}

Node nodeFromJson(const QJsonObject &o, bool legacy)
{
    Node n;
    n.id = o.value(QStringLiteral("id")).toString();
    if (n.id.isEmpty())
        n.id = ConnectionStore::newId();
    n.name = o.value(QStringLiteral("name")).toString();
    if (o.value(QStringLiteral("type")).toString() == QLatin1String("connection")) {
        n.kind = Node::Kind::Host;
        n.conn = connectionFromJson(o.value(QStringLiteral("connection")).toObject(), legacy);
        if (n.name.isEmpty())
            n.name = n.conn.name;
    } else {
        n.kind = Node::Kind::Folder;
        n.expanded = o.value(QStringLiteral("expanded")).toBool(true);
        for (const QJsonValue &v : o.value(QStringLiteral("children")).toArray())
            n.children.append(nodeFromJson(v.toObject(), legacy));
    }
    return n;
}

Node *findNodeRec(Node &node, const QString &id)
{
    if (node.id == id)
        return &node;
    for (Node &child : node.children) {
        if (Node *hit = findNodeRec(child, id))
            return hit;
    }
    return nullptr;
}

const Node *findNodeRec(const Node &node, const QString &id)
{
    if (node.id == id)
        return &node;
    for (const Node &child : node.children) {
        if (const Node *hit = findNodeRec(child, id))
            return hit;
    }
    return nullptr;
}

void collectRec(const Node &node, QVector<const Connection *> &out)
{
    if (node.kind == Node::Kind::Host) {
        out.append(&node.conn);
        return;
    }
    for (const Node &child : node.children)
        collectRec(child, out);
}

void clearRec(Node &node)
{
    if (node.kind == Node::Kind::Host)
        node.conn.clearSensitive();
    for (Node &child : node.children)
        clearRec(child);
}

} // namespace

void Connection::clearSensitive()
{
    crypto::cleanse(password);
    crypto::cleanse(passphrase);
    crypto::cleanse(privateKey);
}

ConnectionStore::ConnectionStore()
{
    reset();
}

void ConnectionStore::reset()
{
    clearSensitive();
    root = Node{};
    root.kind = Node::Kind::Folder;
    root.id = QStringLiteral("root");
    root.name = QStringLiteral("Connessioni");
    root.expanded = true;
}

QString ConnectionStore::newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QJsonObject ConnectionStore::toJson() const
{
    QJsonObject document;
    document[QStringLiteral("version")] = 2;
    document[QStringLiteral("root")] = nodeToJson(root);
    return document;
}

bool ConnectionStore::fromJson(const QJsonObject &document, QString *error)
{
    reset();
    const QJsonObject rootObject = document.value(QStringLiteral("root")).toObject();
    if (rootObject.isEmpty()) {
        if (error)
            *error = QStringLiteral("L'archivio non contiene un albero di connessioni valido.");
        return false;
    }

    Node loaded = nodeFromJson(rootObject, false);
    if (loaded.kind != Node::Kind::Folder) {
        if (error)
            *error = QStringLiteral("Radice dell'archivio non valida.");
        return false;
    }
    if (loaded.id.isEmpty())
        loaded.id = QStringLiteral("root");
    root = loaded;
    return true;
}

bool ConnectionStore::loadLegacyFile(const QString &path, QString *error)
{
    reset();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Impossibile aprire %1.").arg(path);
        return false;
    }

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (document.isNull()) {
        if (error)
            *error = QStringLiteral("Archivio precedente non valido: %1").arg(parseError.errorString());
        return false;
    }

    const QJsonObject rootObject = document.object().value(QStringLiteral("root")).toObject();
    if (rootObject.isEmpty()) {
        if (error)
            *error = QStringLiteral("Archivio precedente senza radice valida.");
        return false;
    }

    Node loaded = nodeFromJson(rootObject, true);
    if (loaded.kind != Node::Kind::Folder) {
        if (error)
            *error = QStringLiteral("Radice dell'archivio precedente non valida.");
        return false;
    }
    if (loaded.id.isEmpty())
        loaded.id = QStringLiteral("root");
    root = loaded;
    return true;
}

void ConnectionStore::clearSensitive()
{
    clearRec(root);
}

Node *ConnectionStore::findNode(const QString &id)
{
    return findNodeRec(root, id);
}

const Node *ConnectionStore::findNode(const QString &id) const
{
    return findNodeRec(root, id);
}

Node *ConnectionStore::findFolder(const QString &id)
{
    Node *node = findNode(id);
    return (node && node->kind == Node::Kind::Folder) ? node : nullptr;
}

Node *ConnectionStore::parentOf(const QString &id)
{
    Node *direct = findNode(id);
    if (!direct || direct == &root)
        return nullptr;

    std::function<Node *(Node &)> walk = [&](Node &node) -> Node * {
        for (Node &child : node.children) {
            if (child.id == id)
                return &node;
            if (child.kind == Node::Kind::Folder) {
                if (Node *hit = walk(child))
                    return hit;
            }
        }
        return nullptr;
    };
    return walk(root);
}

Connection *ConnectionStore::findConnection(const QString &id)
{
    Node *node = findNode(id);
    return (node && node->kind == Node::Kind::Host) ? &node->conn : nullptr;
}

const Connection *ConnectionStore::findConnection(const QString &id) const
{
    const Node *node = findNode(id);
    return (node && node->kind == Node::Kind::Host) ? &node->conn : nullptr;
}

QString ConnectionStore::folderPath(const QString &folderId) const
{
    QStringList parts;
    std::function<bool(const Node &)> walk = [&](const Node &node) -> bool {
        if (node.id == folderId) {
            if (node.kind == Node::Kind::Folder)
                parts.prepend(node.name);
            return true;
        }
        for (const Node &child : node.children) {
            if (walk(child)) {
                if (node.kind == Node::Kind::Folder)
                    parts.prepend(node.name);
                return true;
            }
        }
        return false;
    };
    walk(root);
    if (parts.size() > 1 && parts.first() == root.name)
        parts.removeFirst();
    return parts.join(QStringLiteral(" / "));
}

void ConnectionStore::collectConnections(QVector<const Connection *> &out) const
{
    out.clear();
    collectRec(root, out);
}
