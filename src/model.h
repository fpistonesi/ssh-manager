#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

enum class AuthType { Password = 0, KeyFile = 1, Agent = 2 };

// Per-connection terminal appearance. Defaults match the classic look.
struct TerminalProfile {
    QString colorScheme = QStringLiteral("DarkPastels");
    QString fontFamily;  // empty: system fixed-width font
    int fontSize = 10;
    int cursorShape = 0;  // 0 block, 1 underline, 2 I-beam
    int historySize = 20000;
    int opacityPercent = 100;
    int margin = 4;
    bool blinkCursor = false;
};

struct TunnelConfig {
    QString localAddress = QStringLiteral("127.0.0.1");
    quint16 localPort = 8080;
    QString remoteHost = QStringLiteral("127.0.0.1");
    quint16 remotePort = 80;
    bool enabled = true;
};

struct Connection {
    QString id;
    QString name;
    QString host;
    quint16 port = 22;
    QString user;
    AuthType auth = AuthType::Password;

    QByteArray password;    // wipeable, encrypted inside the vault
    QByteArray passphrase;  // wipeable, encrypted inside the vault
    QByteArray privateKey;  // imported key material, encrypted inside the vault
    QString keyName;        // original file name, for display
    QString keyPath;        // external key file, used when no key is embedded

    QString jumpId;
    QString notes;
    bool favorite = false;
    QStringList tags;
    QVector<TunnelConfig> tunnels;
    TerminalProfile terminal;
    bool local = false;  // transient local shell, never saved in the vault

    QString displayName() const { return name.isEmpty() ? host : name; }
    QString target() const
    {
        if (local)
            return QStringLiteral("locale");
        return user + QLatin1Char('@') + host + QLatin1Char(':') + QString::number(port);
    }

    void clearSensitive();
};

struct Node {
    enum class Kind { Folder, Host };

    Kind kind = Kind::Folder;
    QString id;
    QString name;
    bool expanded = true;
    QVector<Node> children;
    Connection conn;
};

class ConnectionStore {
public:
    ConnectionStore();

    Node root;

    void reset();

    // Plaintext JSON of the whole store; only ever handed to the encrypted vault.
    QJsonObject toJson() const;
    bool fromJson(const QJsonObject &document, QString *error);

    // Reads a pre-vault connections.json (secrets encrypted with the old local
    // key) and keeps the decrypted data in memory for migration.
    bool loadLegacyFile(const QString &path, QString *error);

    void clearSensitive();

    Node *findNode(const QString &id);
    const Node *findNode(const QString &id) const;
    Node *findFolder(const QString &id);
    Node *parentOf(const QString &id);
    Connection *findConnection(const QString &id);
    const Connection *findConnection(const QString &id) const;

    QString folderPath(const QString &folderId) const;
    void collectConnections(QVector<const Connection *> &out) const;

    static QString newId();
};
