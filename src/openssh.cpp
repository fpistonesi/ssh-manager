#include "openssh.h"

#include "sshcommand.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHostInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>
#include <QVBoxLayout>

namespace openssh {

namespace {

QString localUser()
{
    return qEnvironmentVariable("USER", qEnvironmentVariable("USERNAME"));
}

// Splits the arguments of a configuration line; double quotes group words.
QStringList splitArguments(const QString &text)
{
    QStringList result;
    QString current;
    bool quoted = false;
    bool any = false;
    for (const QChar c : text) {
        if (c == QLatin1Char('"')) {
            quoted = !quoted;
            any = true;
        } else if (!quoted && c.isSpace()) {
            if (any)
                result << current;
            current.clear();
            any = false;
        } else {
            current += c;
            any = true;
        }
    }
    if (any)
        result << current;
    return result;
}

struct Block {
    QStringList patterns; // empty: a Match block, never applied
    QVector<QPair<QString, QString>> values; // lower-case keyword, raw arguments
};

void readFile(const QString &path, QVector<Block> &blocks, int depth, QStringList *warnings)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (warnings && depth > 0)
            warnings->append(QCoreApplication::translate("openssh", "Impossibile leggere %1")
                                 .arg(QDir::toNativeSeparators(path)));
        return;
    }
    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        static const QRegularExpression split(QStringLiteral("^(\\S+?)(?:\\s*=\\s*|\\s+)(.*)$"));
        const QRegularExpressionMatch match = split.match(line);
        if (!match.hasMatch())
            continue;
        const QString keyword = match.captured(1).toLower();
        const QString arguments = match.captured(2).trimmed();

        if (keyword == QLatin1String("host")) {
            Block block;
            block.patterns = splitArguments(arguments);
            blocks.append(block);
        } else if (keyword == QLatin1String("match")) {
            blocks.append(Block()); // conditions are not evaluated: skipped
        } else if (keyword == QLatin1String("include")) {
            if (depth >= 8)
                continue;
            for (QString pattern : splitArguments(arguments)) {
                pattern = expandUserPath(pattern);
                if (QFileInfo(pattern).isRelative())
                    pattern = sshDirectory() + QLatin1Char('/') + pattern;
                const QFileInfo info(pattern);
                const QStringList names =
                    QDir(info.path()).entryList({info.fileName()}, QDir::Files, QDir::Name);
                for (const QString &name : names)
                    readFile(info.path() + QLatin1Char('/') + name, blocks, depth + 1, warnings);
            }
        } else {
            if (blocks.isEmpty())
                blocks.append(Block{{QStringLiteral("*")}, {}}); // before any Host: all hosts
            blocks.last().values.append({keyword, arguments});
        }
    }
}

bool isConcrete(const QString &pattern)
{
    return !pattern.contains(QLatin1Char('*')) && !pattern.contains(QLatin1Char('?'))
           && !pattern.startsWith(QLatin1Char('!'));
}

bool applies(const Block &block, const QString &alias)
{
    bool positive = false;
    for (const QString &pattern : block.patterns) {
        const bool negated = pattern.startsWith(QLatin1Char('!'));
        const QRegularExpression re(
            QRegularExpression::wildcardToRegularExpression(negated ? pattern.mid(1) : pattern,
                                                            QRegularExpression::NonPathWildcardConversion),
            QRegularExpression::CaseInsensitiveOption);
        if (re.match(alias).hasMatch()) {
            if (negated)
                return false;
            positive = true;
        }
    }
    return positive;
}

bool parseEndpoint(const QString &text, QString *address, quint16 *port)
{
    // "port", "address:port", "[v6]:port"
    QString value = text;
    QString bind;
    if (value.startsWith(QLatin1Char('['))) {
        const qsizetype close = value.indexOf(QLatin1Char(']'));
        if (close < 0)
            return false;
        bind = value.mid(1, close - 1);
        value = value.mid(close + 1);
        if (value.startsWith(QLatin1Char(':')))
            value = value.mid(1);
    } else if (value.contains(QLatin1Char(':'))) {
        bind = value.section(QLatin1Char(':'), 0, -2);
        value = value.section(QLatin1Char(':'), -1);
    }
    bool ok = false;
    const uint number = value.toUInt(&ok);
    if (!ok || number == 0 || number > 65535)
        return false;
    *port = quint16(number);
    if (!bind.isEmpty() && address)
        *address = bind;
    return true;
}

QString expandTokens(QString value, const QString &host, const QString &user)
{
    value = expandUserPath(value);
    value.replace(QLatin1String("%d"), QDir::homePath());
    value.replace(QLatin1String("%u"), localUser());
    value.replace(QLatin1String("%h"), host);
    value.replace(QLatin1String("%r"), user);
    value.replace(QLatin1String("%%"), QLatin1String("%"));
    return value;
}

QString findExisting(const ConnectionStore &store, const QString &host, const QString &user,
                     quint16 port)
{
    QVector<const Connection *> connections;
    store.collectConnections(connections);
    for (const Connection *connection : connections) {
        if (connection->host.compare(host, Qt::CaseInsensitive) == 0 && connection->user == user
            && connection->port == port)
            return connection->id;
    }
    return QString();
}

QString quoteValue(const QString &value)
{
    return value.contains(QLatin1Char(' ')) ? QLatin1Char('"') + value + QLatin1Char('"') : value;
}

} // namespace

QString localUserComment()
{
    return localUser() + QLatin1Char('@') + QHostInfo::localHostName();
}

QString sshDirectory()
{
    return QDir::homePath() + QStringLiteral("/.ssh");
}

QString defaultConfigPath()
{
    return sshDirectory() + QStringLiteral("/config");
}

QString toolPath(const QString &name)
{
    QString exe = QStandardPaths::findExecutable(name);
#ifdef Q_OS_WIN
    if (exe.isEmpty()) {
        const QString builtin = qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows"))
                                + QStringLiteral("\\System32\\OpenSSH\\") + name
                                + QStringLiteral(".exe");
        if (QFileInfo::exists(builtin))
            exe = builtin;
    }
#endif
    return exe.isEmpty() ? QString() : QDir::toNativeSeparators(exe);
}

ConnectionStore importConfig(const QString &path, const ConnectionStore &current,
                             QStringList *warnings)
{
    QVector<Block> blocks;
    readFile(path, blocks, 0, warnings);

    QStringList aliases;
    for (const Block &block : std::as_const(blocks)) {
        for (const QString &pattern : block.patterns) {
            if (isConcrete(pattern)) {
                if (!aliases.contains(pattern))
                    aliases << pattern;
                break; // further names on the same line are synonyms
            }
        }
    }

    struct Resolved {
        QHash<QString, QString> single; // first value wins
        QHash<QString, QStringList> multi; // forwards accumulate
    };
    const auto resolve = [&](const QString &alias) {
        Resolved resolved;
        static const QSet<QString> forwards = {QStringLiteral("localforward"),
                                               QStringLiteral("remoteforward"),
                                               QStringLiteral("dynamicforward")};
        for (const Block &block : std::as_const(blocks)) {
            if (!applies(block, alias))
                continue;
            for (const auto &value : block.values) {
                if (forwards.contains(value.first))
                    resolved.multi[value.first] << value.second;
                else if (!resolved.single.contains(value.first))
                    resolved.single.insert(value.first, value.second);
            }
        }
        return resolved;
    };

    ConnectionStore store;
    QHash<QString, QString> idByAlias;
    QHash<QString, QString> proxyJumpByAlias;
    QSet<QString> usedIds;

    const auto makeId = [&](const QString &host, const QString &user, quint16 port) {
        QString id = findExisting(current, host, user, port);
        if (id.isEmpty() || usedIds.contains(id))
            id = ConnectionStore::newId();
        usedIds.insert(id);
        return id;
    };

    for (const QString &alias : std::as_const(aliases)) {
        const Resolved values = resolve(alias);
        Connection connection;
        connection.name = alias;
        const QString hostName = values.single.value(QStringLiteral("hostname"));
        connection.host = hostName.isEmpty() ? alias
                                             : QString(hostName).replace(QLatin1String("%h"), alias);
        connection.user = values.single.value(QStringLiteral("user"), localUser());
        const uint port = values.single.value(QStringLiteral("port")).toUInt();
        connection.port = port > 0 && port <= 65535 ? quint16(port) : 22;

        const QString identity = splitArguments(values.single.value(QStringLiteral("identityfile"))).value(0);
        if (!identity.isEmpty() && identity.compare(QLatin1String("none"), Qt::CaseInsensitive) != 0) {
            connection.auth = AuthType::KeyFile;
            connection.keyPath =
                QDir::toNativeSeparators(expandTokens(identity, connection.host, connection.user));
        } else {
            connection.auth = AuthType::Agent; // ssh's default keys and agent
        }

        for (const QString &forward : values.multi.value(QStringLiteral("localforward"))) {
            const QStringList parts = splitArguments(forward);
            TunnelConfig tunnel;
            tunnel.type = TunnelType::Local;
            if (parts.size() == 2 && parseEndpoint(parts.at(0), &tunnel.localAddress, &tunnel.localPort)
                && parseEndpoint(parts.at(1), &tunnel.remoteHost, &tunnel.remotePort))
                connection.tunnels.append(tunnel);
        }
        for (const QString &forward : values.multi.value(QStringLiteral("remoteforward"))) {
            const QStringList parts = splitArguments(forward);
            TunnelConfig tunnel;
            tunnel.type = TunnelType::Remote;
            tunnel.remoteHost = QStringLiteral("localhost");
            if (parts.size() == 2 && parseEndpoint(parts.at(0), &tunnel.remoteHost, &tunnel.remotePort)
                && parseEndpoint(parts.at(1), &tunnel.localAddress, &tunnel.localPort))
                connection.tunnels.append(tunnel);
        }
        for (const QString &forward : values.multi.value(QStringLiteral("dynamicforward"))) {
            TunnelConfig tunnel;
            tunnel.type = TunnelType::Dynamic;
            if (parseEndpoint(splitArguments(forward).value(0), &tunnel.localAddress, &tunnel.localPort))
                connection.tunnels.append(tunnel);
        }

        if (warnings && values.single.contains(QStringLiteral("proxycommand")))
            warnings->append(QCoreApplication::translate("openssh",
                                                         "%1: ProxyCommand non è supportato, "
                                                         "va riconfigurato come jump host")
                                 .arg(alias));
        const QString jump = values.single.value(QStringLiteral("proxyjump"));
        if (!jump.isEmpty() && jump.compare(QLatin1String("none"), Qt::CaseInsensitive) != 0)
            proxyJumpByAlias.insert(alias, jump);

        connection.id = makeId(connection.host, connection.user, connection.port);
        Node node;
        node.kind = Node::Kind::Host;
        node.id = connection.id;
        node.name = alias;
        node.conn = connection;
        store.root.children.append(node);
        idByAlias.insert(alias, connection.id);
    }

    // ProxyJump "a,b": a first, then b, then the host. jumpId points to the
    // hop closest to the destination.
    QHash<QString, QString> literalHops;
    for (auto it = proxyJumpByAlias.cbegin(); it != proxyJumpByAlias.cend(); ++it) {
        QStringList hopIds;
        for (const QString &hop : it.value().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QString text = hop.trimmed();
            if (idByAlias.contains(text)) {
                hopIds << idByAlias.value(text);
                continue;
            }
            if (!literalHops.contains(text)) {
                QString user;
                QString host;
                quint16 port = 22;
                if (!parseQuickTarget(text.contains(QLatin1Char('@')) ? text : localUser() + QLatin1Char('@') + text,
                                      &user, &host, &port)) {
                    if (warnings)
                        warnings->append(QCoreApplication::translate("openssh",
                                                                     "%1: jump host «%2» non riconosciuto")
                                             .arg(it.key(), text));
                    hopIds.clear();
                    break;
                }
                Node node;
                node.kind = Node::Kind::Host;
                node.name = text;
                node.conn.name = text;
                node.conn.host = host;
                node.conn.user = user;
                node.conn.port = port;
                node.conn.auth = AuthType::Agent;
                node.id = node.conn.id = makeId(host, user, port);
                store.root.children.append(node);
                literalHops.insert(text, node.id);
                // A literal hop goes through the hops listed before it.
                if (!hopIds.isEmpty())
                    store.findConnection(node.id)->jumpId = hopIds.last();
            }
            hopIds << literalHops.value(text);
        }
        if (Connection *connection = store.findConnection(idByAlias.value(it.key()));
            connection && !hopIds.isEmpty() && hopIds.last() != connection->id)
            connection->jumpId = hopIds.last();
        // A configured host used as a later hop keeps its own jump settings:
        // the hops before it cannot be represented.
        if (hopIds.size() > 1 && warnings) {
            const Connection *last = store.findConnection(hopIds.last());
            if (last && last->jumpId != hopIds.at(hopIds.size() - 2))
                warnings->append(QCoreApplication::translate("openssh",
                                                             "%1: catena ProxyJump «%2» ridotta "
                                                             "all'ultimo salto")
                                     .arg(it.key(), it.value()));
        }
    }
    // Hosts used in a ProxyJump are offered as jump hosts in the editor.
    store.markReferencedJumpHosts();
    return store;
}

QString exportConfig(const ConnectionStore &store, const QSet<QString> &hostIds)
{
    QVector<const Connection *> all;
    store.collectConnections(all);
    QHash<QString, QString> aliasById;
    QSet<QString> used;
    for (const Connection *connection : std::as_const(all)) {
        if (!hostIds.contains(connection->id))
            continue;
        QString alias = connection->displayName();
        alias.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("-"));
        alias = alias.trimmed();
        if (alias.isEmpty() || alias == QLatin1String("-"))
            alias = connection->host;
        QString unique = alias;
        for (int n = 2; used.contains(unique.toLower()); ++n)
            unique = alias + QLatin1Char('-') + QString::number(n);
        used.insert(unique.toLower());
        aliasById.insert(connection->id, unique);
    }

    QString text;
    QTextStream out(&text);
    out << "# Generato da SSH Manager il "
        << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
    for (const Connection *connection : std::as_const(all)) {
        if (!aliasById.contains(connection->id))
            continue;
        out << "\nHost " << aliasById.value(connection->id) << "\n";
        if (connection->displayName() != aliasById.value(connection->id))
            out << "    # " << connection->displayName() << "\n";
        out << "    HostName " << connection->host << "\n";
        if (!connection->user.isEmpty())
            out << "    User " << connection->user << "\n";
        if (connection->port != 22)
            out << "    Port " << connection->port << "\n";
        if (connection->auth == AuthType::KeyFile) {
            if (!connection->privateKey.isEmpty())
                out << "    # Chiave incorporata in SSH Manager (" << connection->keyName
                    << "): salvala in un file e indicala qui con IdentityFile\n";
            else if (!connection->keyPath.isEmpty())
                out << "    IdentityFile " << quoteValue(QDir::fromNativeSeparators(connection->keyPath)) << "\n";
        }
        if (!connection->jumpId.isEmpty()) {
            if (aliasById.contains(connection->jumpId)) {
                out << "    ProxyJump " << aliasById.value(connection->jumpId) << "\n";
            } else if (const Connection *jump = store.findConnection(connection->jumpId)) {
                out << "    ProxyJump " << jump->user << '@' << jump->host;
                if (jump->port != 22)
                    out << ':' << jump->port;
                out << "\n";
            }
        }
        for (const TunnelConfig &tunnel : connection->tunnels) {
            if (!tunnel.enabled)
                continue;
            const auto bind = [](const QString &address, quint16 port) {
                const bool loopback = address.isEmpty() || address == QLatin1String("127.0.0.1")
                                      || address == QLatin1String("localhost");
                return loopback ? QString::number(port) : address + QLatin1Char(':') + QString::number(port);
            };
            switch (tunnel.type) {
            case TunnelType::Local:
                out << "    LocalForward " << bind(tunnel.localAddress, tunnel.localPort) << ' '
                    << tunnel.remoteHost << ':' << tunnel.remotePort << "\n";
                break;
            case TunnelType::Remote:
                out << "    RemoteForward " << bind(tunnel.remoteHost, tunnel.remotePort) << ' '
                    << tunnel.localAddress << ':' << tunnel.localPort << "\n";
                break;
            case TunnelType::Dynamic:
                out << "    DynamicForward " << bind(tunnel.localAddress, tunnel.localPort) << "\n";
                break;
            }
        }
    }
    return text;
}

QStringList publicKeyFiles()
{
    QStringList files;
    const QDir dir(sshDirectory());
    for (const QString &name : dir.entryList({QStringLiteral("*.pub")}, QDir::Files, QDir::Name))
        files << QDir::toNativeSeparators(dir.filePath(name));
    return files;
}

QString readPublicKey(const QString &path, QString *error)
{
    QFile file(path);
    if (file.size() > 16 * 1024 || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        *error = QCoreApplication::translate("openssh", "Impossibile leggere %1")
                     .arg(QDir::toNativeSeparators(path));
        return QString();
    }
    static const QRegularExpression keyLine(
        QStringLiteral("^(ssh-[a-z0-9-]+|ecdsa-sha2-[a-z0-9-]+|sk-[a-z0-9@.-]+)\\s+[A-Za-z0-9+/=]+(\\s+.*)?$"));
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.isEmpty())
            continue;
        if (keyLine.match(line).hasMatch() && !line.contains(QLatin1Char('\'')))
            return line;
        break;
    }
    *error = QCoreApplication::translate("openssh", "%1 non contiene una chiave pubblica OpenSSH.")
                 .arg(QDir::toNativeSeparators(path));
    return QString();
}

QString authorizedKeysCommand()
{
    // Run by sh whatever the login shell; the key arrives on stdin, so it is
    // never part of a command line. A missing final newline is added first.
    return QStringLiteral(
        "sh -c 'umask 077; mkdir -p ~/.ssh && touch ~/.ssh/authorized_keys || exit 1; "
        "IFS= read -r key || exit 2; f=~/.ssh/authorized_keys; "
        "if grep -qxF \"$key\" \"$f\"; then echo SSHMANAGER_KEY_PRESENT; exit 0; fi; "
        "if [ -s \"$f\" ] && [ -n \"$(tail -c 1 \"$f\")\" ]; then echo >> \"$f\"; fi; "
        "printf \"%s\\n\" \"$key\" >> \"$f\" && echo SSHMANAGER_KEY_ADDED'");
}

} // namespace openssh

// --- KeyGenDialog -----------------------------------------------------------

KeyGenDialog::KeyGenDialog(const QString &connectionName, QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Genera chiave SSH"));
    resize(560, 0);
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout();

    type_ = new QComboBox(this);
    type_->addItem(tr("Ed25519 (consigliata)"), QStringLiteral("ed25519"));
    type_->addItem(tr("RSA 4096 (per server datati)"), QStringLiteral("rsa"));
    form->addRow(tr("Tipo"), type_);

    QString base = connectionName;
    base.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("_"));
    const auto suggestedPath = [this, base] {
        const QString type = type_->currentData().toString();
        QString name = QStringLiteral("id_") + type;
        if (!base.isEmpty())
            name += QLatin1Char('_') + base;
        QString path = openssh::sshDirectory() + QLatin1Char('/') + name;
        for (int n = 2; QFileInfo::exists(path) || QFileInfo::exists(path + QStringLiteral(".pub")); ++n)
            path = openssh::sshDirectory() + QLatin1Char('/') + name + QLatin1Char('_') + QString::number(n);
        return QDir::toNativeSeparators(path);
    };

    auto *pathRow = new QHBoxLayout();
    path_ = new QLineEdit(suggestedPath(), this);
    auto *browse = new QPushButton(tr("Sfoglia…"), this);
    pathRow->addWidget(path_, 1);
    pathRow->addWidget(browse);
    form->addRow(tr("File della chiave privata"), pathRow);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getSaveFileName(this, tr("Chiave privata"), path_->text());
        if (!file.isEmpty())
            path_->setText(QDir::toNativeSeparators(file));
    });
    connect(type_, &QComboBox::currentIndexChanged, this, [this, suggestedPath] {
        path_->setText(suggestedPath());
    });

    passphrase_ = new QLineEdit(this);
    passphrase_->setEchoMode(QLineEdit::Password);
    passphrase_->setPlaceholderText(tr("facoltativa, consigliata"));
    form->addRow(tr("Passphrase"), passphrase_);
    confirm_ = new QLineEdit(this);
    confirm_->setEchoMode(QLineEdit::Password);
    form->addRow(tr("Conferma passphrase"), confirm_);

    comment_ = new QLineEdit(openssh::localUserComment(), this);
    form->addRow(tr("Commento"), comment_);
    layout->addLayout(form);

    if (!connectionName.isEmpty()) {
        useForConnection_ = new QCheckBox(tr("Usa questa chiave per «%1» (la passphrase viene "
                                             "salvata nell'archivio)")
                                              .arg(connectionName),
                                          this);
        useForConnection_->setChecked(true);
        layout->addWidget(useForConnection_);
        copyToHost_ = new QCheckBox(tr("Copia subito la chiave pubblica sull'host (con le "
                                       "credenziali attuali)"),
                                    this);
        copyToHost_->setChecked(true);
        layout->addWidget(copyToHost_);
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Genera"));
    connect(buttons, &QDialogButtonBox::accepted, this, &KeyGenDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QByteArray KeyGenDialog::passphrase() const
{
    return passphrase_->text().toUtf8();
}

bool KeyGenDialog::useForConnection() const
{
    return useForConnection_ && useForConnection_->isChecked();
}

bool KeyGenDialog::copyToHost() const
{
    return copyToHost_ && copyToHost_->isChecked();
}

void KeyGenDialog::accept()
{
    const QString path = QDir::fromNativeSeparators(path_->text().trimmed());
    if (path.isEmpty())
        return;
    if (QFileInfo::exists(path) || QFileInfo::exists(path + QStringLiteral(".pub"))) {
        QMessageBox::warning(this, windowTitle(),
                             tr("Il file esiste già: scegli un altro nome per non sovrascrivere "
                                "una chiave esistente."));
        return;
    }
    if (passphrase_->text() != confirm_->text()) {
        QMessageBox::warning(this, windowTitle(), tr("Le due passphrase non coincidono."));
        confirm_->setFocus();
        return;
    }
    const QString keygen = openssh::toolPath(QStringLiteral("ssh-keygen"));
    if (keygen.isEmpty()) {
        QMessageBox::critical(this, windowTitle(),
                              tr("ssh-keygen non trovato: installa il client OpenSSH."));
        return;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());

    QStringList args{QStringLiteral("-q"), QStringLiteral("-t"), type_->currentData().toString()};
    if (type_->currentData().toString() == QLatin1String("rsa"))
        args << QStringLiteral("-b") << QStringLiteral("4096");
    args << QStringLiteral("-f") << QDir::toNativeSeparators(path) << QStringLiteral("-N")
         << passphrase_->text() << QStringLiteral("-C") << comment_->text().trimmed();
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(keygen, args);
    process.closeWriteChannel();
    if (!process.waitForFinished(60000) || process.exitStatus() != QProcess::NormalExit
        || process.exitCode() != 0 || !QFileInfo::exists(path)) {
        process.kill();
        QMessageBox::critical(this, windowTitle(),
                              tr("ssh-keygen non è riuscito a creare la chiave.\n\n%1")
                                  .arg(QString::fromLocal8Bit(process.readAll()).trimmed()));
        return;
    }
    generated_ = QDir::toNativeSeparators(path);
    QDialog::accept();
}

// --- PublicKeyDialog --------------------------------------------------------

PublicKeyDialog::PublicKeyDialog(const QString &target, const QString &suggested, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Copia la chiave pubblica sull'host"));
    resize(600, 0);
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("La chiave pubblica scelta viene aggiunta a "
                                "~/.ssh/authorized_keys di %1 (se non c'è già), accedendo con le "
                                "credenziali attuali della connessione.")
                                 .arg(target),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *row = new QHBoxLayout();
    file_ = new QComboBox(this);
    file_->setEditable(true);
    file_->addItems(openssh::publicKeyFiles());
    if (!suggested.isEmpty()) {
        const int index = file_->findText(QDir::toNativeSeparators(suggested), Qt::MatchFixedString);
        if (index >= 0)
            file_->setCurrentIndex(index);
        else
            file_->setCurrentText(QDir::toNativeSeparators(suggested));
    }
    auto *browse = new QPushButton(tr("Sfoglia…"), this);
    row->addWidget(file_, 1);
    row->addWidget(browse);
    layout->addLayout(row);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(
            this, tr("Chiave pubblica"), openssh::sshDirectory(),
            tr("Chiavi pubbliche (*.pub);;Tutti i file (*)"));
        if (!file.isEmpty())
            file_->setCurrentText(QDir::toNativeSeparators(file));
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Copia sull'host"));
    connect(buttons, &QDialogButtonBox::accepted, this, &PublicKeyDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QString PublicKeyDialog::path() const
{
    return file_->currentText().trimmed();
}

void PublicKeyDialog::accept()
{
    QString error;
    key_ = openssh::readPublicKey(path(), &error);
    if (key_.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    QDialog::accept();
}
