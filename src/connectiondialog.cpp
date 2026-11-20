#include "connectiondialog.h"

#include "crypto.h"
#include "terminaldialog.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QCollator>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

ConnectionDialog::ConnectionDialog(const ConnectionStore &store, QWidget *parent)
    : QDialog(parent), store_(store)
{
    setWindowTitle(tr("Connessione SSH"));
    setMinimumWidth(660);
    resize(760, 460);

    auto *layout = new QVBoxLayout(this);
    auto *tabs = new QTabWidget(this);

    auto *serverPage = new QWidget(tabs);
    auto *serverPageLayout = new QVBoxLayout(serverPage);
    auto *networkPage = new QWidget(tabs);
    auto *networkPageLayout = new QVBoxLayout(networkPage);
    auto *terminalPage = new QWidget(tabs);
    auto *terminalPageLayout = new QVBoxLayout(terminalPage);

    tabs->addTab(serverPage, tr("Server"));
    tabs->addTab(networkPage, tr("Rete"));
    tabs->addTab(terminalPage, tr("Terminale"));
    layout->addWidget(tabs, 1);

    auto *serverGroup = new QGroupBox(tr("Server"), this);
    auto *serverForm = new QFormLayout(serverGroup);
    name_ = new QLineEdit(serverGroup);
    name_->setPlaceholderText(tr("Nome visualizzato (opzionale)"));
    folder_ = new QComboBox(serverGroup);
    host_ = new QLineEdit(serverGroup);
    host_->setPlaceholderText(tr("host o indirizzo IP"));
    port_ = new QSpinBox(serverGroup);
    port_->setRange(1, 65535);
    port_->setValue(22);
    user_ = new QLineEdit(serverGroup);
    user_->setText(qEnvironmentVariable("USER"));
    serverForm->addRow(tr("Nome"), name_);
    serverForm->addRow(tr("Cartella"), folder_);
    serverForm->addRow(tr("Host"), host_);
    serverForm->addRow(tr("Porta"), port_);
    serverForm->addRow(tr("Utente"), user_);
    favorite_ = new QCheckBox(tr("Segna come preferito"), serverGroup);
    tags_ = new QLineEdit(serverGroup);
    tags_->setPlaceholderText(tr("tag separati da virgola, es. produzione, web"));
    jumpHost_ = new QCheckBox(tr("Usa come jump host"), serverGroup);
    jumpHost_->setToolTip(tr("Proponi questa connessione nell'elenco \"Passa per\" delle altre "
                             "connessioni."));
    auto *flagsRow = new QHBoxLayout;
    flagsRow->addWidget(favorite_);
    flagsRow->addSpacing(16);
    flagsRow->addWidget(jumpHost_);
    flagsRow->addStretch(1);
    serverForm->addRow(flagsRow);
    serverForm->addRow(tr("Tag"), tags_);
    serverPageLayout->addWidget(serverGroup);

    auto *authGroup = new QGroupBox(tr("Autenticazione"), this);
    authForm_ = new QFormLayout(authGroup);
    auth_ = new QComboBox(authGroup);
    auth_->addItem(tr("Password"), static_cast<int>(AuthType::Password));
    auth_->addItem(tr("Chiave SSH"), static_cast<int>(AuthType::KeyFile));
    auth_->addItem(tr("Agent SSH"), static_cast<int>(AuthType::Agent));
    authForm_->addRow(tr("Metodo"), auth_);

    auto *passwordWidget = new QWidget(authGroup);
    auto *passwordLayout = new QHBoxLayout(passwordWidget);
    passwordLayout->setContentsMargins(0, 0, 0, 0);
    password_ = new QLineEdit(passwordWidget);
    password_->setEchoMode(QLineEdit::Password);
    showPassword_ = new QCheckBox(tr("Mostra"), passwordWidget);
    passwordLayout->addWidget(password_, 1);
    passwordLayout->addWidget(showPassword_);
    passwordRow_ = passwordWidget;
    authForm_->addRow(tr("Password"), passwordWidget);

    auto *keyWidget = new QWidget(authGroup);
    auto *keyLayout = new QHBoxLayout(keyWidget);
    keyLayout->setContentsMargins(0, 0, 0, 0);
    keyPath_ = new QLineEdit(keyWidget);
    keyPath_->setPlaceholderText(tr("~/.ssh/id_ed25519"));
    auto *browseButton = new QPushButton(tr("Sfoglia…"), keyWidget);
    keyLayout->addWidget(keyPath_, 1);
    keyLayout->addWidget(browseButton);
    keyRow_ = keyWidget;
    authForm_->addRow(tr("Chiave privata"), keyWidget);

    auto *keyStatusWidget = new QWidget(authGroup);
    auto *keyStatusLayout = new QHBoxLayout(keyStatusWidget);
    keyStatusLayout->setContentsMargins(0, 0, 0, 0);
    keyStatus_ = new QLabel(keyStatusWidget);
    keyStatus_->setWordWrap(true);
    removeKeyButton_ = new QPushButton(tr("Rimuovi"), keyStatusWidget);
    auto *importKeyButton = new QPushButton(tr("Importa nel database…"), keyStatusWidget);
    keyStatusLayout->addWidget(keyStatus_, 1);
    keyStatusLayout->addWidget(importKeyButton);
    keyStatusLayout->addWidget(removeKeyButton_);
    keyStatusRow_ = keyStatusWidget;
    authForm_->addRow(tr("Chiave archiviata"), keyStatusWidget);

    passphrase_ = new QLineEdit(authGroup);
    passphrase_->setEchoMode(QLineEdit::Password);
    passphrase_->setPlaceholderText(tr("Passphrase della chiave (se presente)"));
    authForm_->addRow(tr("Passphrase"), passphrase_);

    auto *agentLabel = new QLabel(
        tr("Usa le chiavi già caricate in ssh-agent: nessun segreto viene salvato."), authGroup);
    agentLabel->setWordWrap(true);
    agentRow_ = agentLabel;
    authForm_->addRow(agentLabel);
    serverPageLayout->addWidget(authGroup);
    serverPageLayout->addStretch(1);

    auto *jumpGroup = new QGroupBox(tr("Jump host"), this);
    auto *jumpForm = new QFormLayout(jumpGroup);
    jump_ = new QComboBox(jumpGroup);
    jumpForm->addRow(tr("Passa per"), jump_);
    auto *jumpHint = new QLabel(
        tr("Il jump host può a sua volta avere un jump host. I tunnel vengono creati attraverso "
           "tutta la catena."),
        jumpGroup);
    jumpHint->setWordWrap(true);
    jumpForm->addRow(jumpHint);
    networkPageLayout->addWidget(jumpGroup);

    auto *terminalGroup = new QGroupBox(tr("Terminale"), this);
    auto *terminalLayout = new QHBoxLayout(terminalGroup);
    terminalSummary_ = new QLabel(terminalGroup);
    terminalSummary_->setWordWrap(true);
    auto *terminalButton = new QPushButton(tr("Personalizza terminale…"), terminalGroup);
    terminalLayout->addWidget(terminalSummary_, 1);
    terminalLayout->addWidget(terminalButton);
    terminalPageLayout->addWidget(terminalGroup);

    auto *tunnelGroup = new QGroupBox(tr("Tunnel (port forwarding)"), this);
    auto *tunnelLayout = new QVBoxLayout(tunnelGroup);
    tunnels_ = new QTableWidget(0, TunnelColumnCount, tunnelGroup);
    tunnels_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tunnels_->setHorizontalHeaderLabels({tr("Attivo"), tr("Tipo"), tr("Indirizzo locale"),
                                         tr("Porta locale"), tr("Host remoto"),
                                         tr("Porta remota")});
    QHeaderView *tunnelHeader = tunnels_->horizontalHeader();
    tunnelHeader->setSectionResizeMode(TunnelEnabledColumn, QHeaderView::ResizeToContents);
    tunnelHeader->setSectionResizeMode(TunnelTypeColumn, QHeaderView::ResizeToContents);
    tunnelHeader->setSectionResizeMode(TunnelLocalAddressColumn, QHeaderView::Stretch);
    tunnelHeader->setSectionResizeMode(TunnelLocalPortColumn, QHeaderView::ResizeToContents);
    tunnelHeader->setSectionResizeMode(TunnelRemoteHostColumn, QHeaderView::Stretch);
    tunnelHeader->setSectionResizeMode(TunnelRemotePortColumn, QHeaderView::ResizeToContents);
    tunnels_->verticalHeader()->setVisible(false);
    tunnelLayout->addWidget(tunnels_);

    auto *tunnelHint = new QLabel(
        tr("<b>Locale (-L)</b>: apre la porta locale e la inoltra a host:porta remoti, "
           "raggiunti dal server. <b>Remoto (-R)</b>: apre la porta remota sul server "
           "(host remoto = indirizzo di ascolto, vuoto per il default) e la inoltra a "
           "indirizzo:porta locali. <b>Dinamico (-D)</b>: proxy SOCKS sulla porta locale."),
        tunnelGroup);
    tunnelHint->setWordWrap(true);
    tunnelHint->setEnabled(false);
    tunnelLayout->addWidget(tunnelHint);

    auto *tunnelButtons = new QHBoxLayout();
    auto *addButton = new QPushButton(tr("Aggiungi tunnel"), tunnelGroup);
    auto *removeButton = new QPushButton(tr("Rimuovi"), tunnelGroup);
    tunnelButtons->addWidget(addButton);
    tunnelButtons->addWidget(removeButton);
    tunnelButtons->addStretch(1);
    tunnelLayout->addLayout(tunnelButtons);
    networkPageLayout->addWidget(tunnelGroup);
    networkPageLayout->addStretch(1);

    auto *notesGroup = new QGroupBox(tr("Note"), this);
    auto *notesLayout = new QVBoxLayout(notesGroup);
    notes_ = new QPlainTextEdit(notesGroup);
    notes_->setMaximumHeight(70);
    notesLayout->addWidget(notes_);

    auto *startupGroup = new QGroupBox(tr("Comando all'avvio"), this);
    auto *startupLayout = new QVBoxLayout(startupGroup);
    startupCommand_ = new QLineEdit(startupGroup);
    startupCommand_->setPlaceholderText(tr("es. cd /var/www && git status"));
    startupCommand_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    startupLayout->addWidget(startupCommand_);
    auto *startupHint = new QLabel(
        tr("Eseguito sul server all'apertura della sessione, poi si apre la shell interattiva. "
           "Puoi usare ${host}, ${user}, ${port} e ${name}."),
        startupGroup);
    startupHint->setWordWrap(true);
    startupHint->setEnabled(false);
    startupLayout->addWidget(startupHint);
    terminalPageLayout->addWidget(startupGroup);
    terminalPageLayout->addWidget(notesGroup);
    terminalPageLayout->addStretch(1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &ConnectionDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(auth_, &QComboBox::currentIndexChanged, this, &ConnectionDialog::updateAuthRows);
    connect(showPassword_, &QCheckBox::toggled, this, [this](bool shown) {
        password_->setEchoMode(shown ? QLineEdit::Normal : QLineEdit::Password);
    });
    connect(browseButton, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(
            this, tr("Seleziona chiave privata"), QDir::homePath() + QStringLiteral("/.ssh"));
        if (!file.isEmpty())
            keyPath_->setText(file);
    });
    connect(importKeyButton, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(
            this, tr("Importa chiave privata nel database"),
            QDir::homePath() + QStringLiteral("/.ssh"));
        if (file.isEmpty())
            return;

        QFile keyFile(file);
        if (!keyFile.open(QIODevice::ReadOnly) || keyFile.size() > 512 * 1024) {
            QMessageBox::warning(this, tr("Chiave non valida"),
                                 tr("Impossibile leggere il file della chiave (massimo 512 KB)."));
            return;
        }
        privateKey_ = keyFile.readAll();
        keyName_ = QFileInfo(file).fileName();
        keyPath_->clear();
        updateKeyStatus();
    });
    connect(removeKeyButton_, &QPushButton::clicked, this, [this] {
        crypto::cleanse(privateKey_);
        keyName_.clear();
        updateKeyStatus();
    });
    connect(terminalButton, &QPushButton::clicked, this, [this] {
        TerminalDialog dialog(terminal_, this);
        const QString title = name_->text().trimmed();
        if (!title.isEmpty())
            dialog.setWindowTitle(tr("Terminale — %1").arg(title));
        if (dialog.exec() == QDialog::Accepted) {
            terminal_ = dialog.profile();
            updateTerminalSummary();
        }
    });
    connect(addButton, &QPushButton::clicked, this, [this] {
        TunnelConfig tunnel;
        addTunnelRow(tunnel);
    });
    connect(removeButton, &QPushButton::clicked, this, [this] {
        // Collect every selected cell's row, not just column 0: with a single
        // cell selected, selectedRows() would be empty and nothing happened.
        QSet<int> selectedRows;
        if (const QItemSelectionModel *selection = tunnels_->selectionModel()) {
            for (const QModelIndex &index : selection->selectedIndexes())
                selectedRows.insert(index.row());
        }
        if (selectedRows.isEmpty() && tunnels_->currentRow() >= 0)
            selectedRows.insert(tunnels_->currentRow());

        QList<int> rows(selectedRows.begin(), selectedRows.end());
        std::sort(rows.begin(), rows.end(), std::greater<int>());
        for (int row : rows)
            tunnels_->removeRow(row);
    });

    populateFolders();
    populateJumpHosts(QString());
    updateAuthRows();
    updateKeyStatus();
    updateTerminalSummary();
}

void ConnectionDialog::updateTerminalSummary()
{
    const QString font = terminal_.fontFamily.isEmpty()
                             ? tr("font fisso di sistema")
                             : terminal_.fontFamily;
    terminalSummary_->setText(tr("Schema %1 · %2 · %3 pt · cronologia %4 righe")
                                  .arg(terminal_.colorScheme, font)
                                  .arg(terminal_.fontSize)
                                  .arg(terminal_.historySize));
}

ConnectionDialog::~ConnectionDialog()
{
    crypto::cleanse(privateKey_);
}

void ConnectionDialog::populateFolders()
{
    const QString current = folder_->currentData().toString();
    folder_->clear();
    folder_->addItem(tr("Livello principale"), store_.root.id);

    std::function<void(const Node &, const QString &)> walk = [&](const Node &node,
                                                                 const QString &prefix) {
        for (const Node &child : node.children) {
            if (child.kind != Node::Kind::Folder)
                continue;
            const QString label = prefix.isEmpty() ? child.name : prefix + QStringLiteral(" / ") + child.name;
            folder_->addItem(label, child.id);
            walk(child, label);
        }
    };
    walk(store_.root, QString());

    const int index = folder_->findData(current);
    if (index >= 0)
        folder_->setCurrentIndex(index);
}

void ConnectionDialog::populateJumpHosts(const QString &excludeId, const QString &keepId)
{
    const QString current = keepId.isEmpty() ? jump_->currentData().toString() : keepId;
    jump_->clear();
    jump_->addItem(tr("Nessuno (connessione diretta)"), QString());

    // Only connections marked as jump hosts, plus the one already in use so
    // that editing never drops it, in natural alphabetical order.
    QVector<const Connection *> connections;
    store_.collectConnections(connections);
    QVector<const Connection *> candidates;
    for (const Connection *candidate : connections) {
        if (!excludeId.isEmpty() && candidate->id == excludeId)
            continue;
        if (candidate->jumpHost || (!current.isEmpty() && candidate->id == current))
            candidates.append(candidate);
    }
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::stable_sort(candidates.begin(), candidates.end(),
                     [&collator](const Connection *a, const Connection *b) {
                         return collator.compare(a->displayName(), b->displayName()) < 0;
                     });
    for (const Connection *candidate : candidates) {
        jump_->addItem(QStringLiteral("%1 (%2)").arg(candidate->displayName(), candidate->target()),
                       candidate->id);
    }

    const int index = jump_->findData(current);
    if (index >= 0)
        jump_->setCurrentIndex(index);
}

void ConnectionDialog::updateAuthRows()
{
    const auto method = static_cast<AuthType>(auth_->currentData().toInt());
    const bool password = method == AuthType::Password;
    const bool key = method == AuthType::KeyFile;

    authForm_->setRowVisible(passwordRow_, password);
    authForm_->setRowVisible(keyRow_, key);
    authForm_->setRowVisible(keyStatusRow_, key);
    authForm_->setRowVisible(passphrase_, key);
    authForm_->setRowVisible(agentRow_, method == AuthType::Agent);
}

void ConnectionDialog::updateKeyStatus()
{
    if (privateKey_.isEmpty()) {
        keyStatus_->setText(tr("Nessuna chiave importata: verrà usato il file indicato sopra."));
        removeKeyButton_->setEnabled(false);
        return;
    }

    keyStatus_->setText(tr("%1 (%2 byte) cifrata nel database")
                            .arg(keyName_.isEmpty() ? tr("chiave privata") : keyName_)
                            .arg(privateKey_.size()));
    removeKeyButton_->setEnabled(true);
}

void ConnectionDialog::addTunnelRow(const TunnelConfig &tunnel)
{
    const int row = tunnels_->rowCount();
    tunnels_->insertRow(row);

    auto *active = new QTableWidgetItem();
    active->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    active->setCheckState(tunnel.enabled ? Qt::Checked : Qt::Unchecked);
    tunnels_->setItem(row, TunnelEnabledColumn, active);

    auto *type = new QComboBox(tunnels_);
    type->addItem(tr("Locale (-L)"), int(TunnelType::Local));
    type->addItem(tr("Remoto (-R)"), int(TunnelType::Remote));
    type->addItem(tr("Dinamico (-D)"), int(TunnelType::Dynamic));
    type->setCurrentIndex(type->findData(int(tunnel.type)));
    tunnels_->setCellWidget(row, TunnelTypeColumn, type);

    tunnels_->setItem(row, TunnelLocalAddressColumn, new QTableWidgetItem(tunnel.localAddress));
    tunnels_->setItem(row, TunnelLocalPortColumn,
                      new QTableWidgetItem(QString::number(tunnel.localPort)));
    tunnels_->setItem(row, TunnelRemoteHostColumn, new QTableWidgetItem(tunnel.remoteHost));
    tunnels_->setItem(row, TunnelRemotePortColumn,
                      new QTableWidgetItem(QString::number(tunnel.remotePort)));

    // Rows move when others are removed: look the row up from the combo.
    connect(type, &QComboBox::currentIndexChanged, this, [this, type] {
        for (int r = 0; r < tunnels_->rowCount(); ++r) {
            if (tunnels_->cellWidget(r, TunnelTypeColumn) == type) {
                updateTunnelRow(r);
                return;
            }
        }
    });
    updateTunnelRow(row);
}

TunnelType ConnectionDialog::tunnelType(int row) const
{
    if (auto *combo = qobject_cast<QComboBox *>(tunnels_->cellWidget(row, TunnelTypeColumn)))
        return static_cast<TunnelType>(combo->currentData().toInt());
    return TunnelType::Local;
}

void ConnectionDialog::updateTunnelRow(int row)
{
    const TunnelType type = tunnelType(row);
    const bool usesRemote = type != TunnelType::Dynamic;
    for (int column : {int(TunnelRemoteHostColumn), int(TunnelRemotePortColumn)}) {
        QTableWidgetItem *item = tunnels_->item(row, column);
        if (!item)
            continue;
        item->setFlags(usesRemote ? (Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable)
                                  : Qt::ItemIsSelectable);
    }

    QString localTip;
    QString remoteTip;
    switch (type) {
    case TunnelType::Local:
        localTip = tr("Indirizzo e porta su cui ascoltare su questo computer");
        remoteTip = tr("Destinazione raggiunta dal server");
        break;
    case TunnelType::Remote:
        localTip = tr("Destinazione raggiunta da questo computer");
        remoteTip = tr("Indirizzo e porta su cui ascoltare sul server (indirizzo vuoto = default "
                       "del server)");
        break;
    case TunnelType::Dynamic:
        localTip = tr("Indirizzo e porta del proxy SOCKS su questo computer");
        remoteTip = tr("Non usato dal proxy SOCKS");
        break;
    }
    for (int column : {int(TunnelLocalAddressColumn), int(TunnelLocalPortColumn)}) {
        if (QTableWidgetItem *item = tunnels_->item(row, column))
            item->setToolTip(localTip);
    }
    for (int column : {int(TunnelRemoteHostColumn), int(TunnelRemotePortColumn)}) {
        if (QTableWidgetItem *item = tunnels_->item(row, column))
            item->setToolTip(remoteTip);
    }
}

QVector<TunnelConfig> ConnectionDialog::readTunnels() const
{
    QVector<TunnelConfig> list;
    for (int row = 0; row < tunnels_->rowCount(); ++row) {
        TunnelConfig tunnel;
        tunnel.type = tunnelType(row);
        if (auto *active = tunnels_->item(row, TunnelEnabledColumn))
            tunnel.enabled = active->checkState() == Qt::Checked;
        if (auto *item = tunnels_->item(row, TunnelLocalAddressColumn))
            tunnel.localAddress = item->text().trimmed();
        if (auto *item = tunnels_->item(row, TunnelLocalPortColumn))
            tunnel.localPort = static_cast<quint16>(item->text().toUInt());
        if (auto *item = tunnels_->item(row, TunnelRemoteHostColumn))
            tunnel.remoteHost = item->text().trimmed();
        if (auto *item = tunnels_->item(row, TunnelRemotePortColumn))
            tunnel.remotePort = static_cast<quint16>(item->text().toUInt());
        list.append(tunnel);
    }
    return list;
}

void ConnectionDialog::setConnection(const Connection &connection)
{
    connectionId_ = connection.id;
    name_->setText(connection.name);
    host_->setText(connection.host);
    port_->setValue(connection.port > 0 ? connection.port : 22);
    if (!connection.user.isEmpty())
        user_->setText(connection.user);
    password_->setText(QString::fromUtf8(connection.password));
    keyPath_->setText(connection.keyPath);
    passphrase_->setText(QString::fromUtf8(connection.passphrase));
    privateKey_ = connection.privateKey;
    keyName_ = connection.keyName;
    updateKeyStatus();
    notes_->setPlainText(connection.notes);
    startupCommand_->setText(connection.startupCommand);
    favorite_->setChecked(connection.favorite);
    tags_->setText(connection.tags.join(QStringLiteral(", ")));
    terminal_ = connection.terminal;
    updateTerminalSummary();

    const int authIndex = auth_->findData(static_cast<int>(connection.auth));
    if (authIndex >= 0)
        auth_->setCurrentIndex(authIndex);

    tunnels_->setRowCount(0);
    for (const TunnelConfig &tunnel : connection.tunnels)
        addTunnelRow(tunnel);

    jumpHost_->setChecked(connection.jumpHost);
    populateJumpHosts(connection.id, connection.jumpId);
    const int jumpIndex = jump_->findData(connection.jumpId);
    if (jumpIndex >= 0)
        jump_->setCurrentIndex(jumpIndex);

    updateAuthRows();
}

void ConnectionDialog::setFolderId(const QString &folderId)
{
    const int index = folder_->findData(folderId);
    if (index >= 0)
        folder_->setCurrentIndex(index);
}

QString ConnectionDialog::folderId() const
{
    return folder_->currentData().toString();
}

Connection ConnectionDialog::connection() const
{
    Connection connection;
    connection.id = connectionId_;
    connection.name = name_->text().trimmed();
    connection.host = host_->text().trimmed();
    connection.port = static_cast<quint16>(port_->value());
    connection.user = user_->text().trimmed();
    connection.auth = static_cast<AuthType>(auth_->currentData().toInt());
    connection.password = password_->text().toUtf8();
    connection.keyPath = keyPath_->text().trimmed();
    connection.passphrase = passphrase_->text().toUtf8();
    connection.privateKey = privateKey_;
    connection.keyName = keyName_;
    connection.jumpId = jump_->currentData().toString();
    connection.notes = notes_->toPlainText();
    connection.startupCommand = startupCommand_->text().trimmed();
    connection.favorite = favorite_->isChecked();
    connection.jumpHost = jumpHost_->isChecked();
    for (const QString &tag : tags_->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString trimmed = tag.trimmed();
        if (!trimmed.isEmpty() && !connection.tags.contains(trimmed, Qt::CaseInsensitive))
            connection.tags.append(trimmed);
    }
    connection.tunnels = readTunnels();
    connection.terminal = terminal_;
    if (connection.name.isEmpty())
        connection.name = connection.host;
    return connection;
}

void ConnectionDialog::accept()
{
    const QString host = host_->text().trimmed();
    const QString user = user_->text().trimmed();

    if (host.isEmpty() || user.isEmpty()) {
        QMessageBox::warning(this, tr("Dati mancanti"),
                             tr("Host e utente sono obbligatori."));
        return;
    }

    static const QRegularExpression unsafe(QStringLiteral("[\\s'\"`$;&|<>()]"));
    if (unsafe.match(host).hasMatch() || unsafe.match(user).hasMatch()) {
        QMessageBox::warning(this, tr("Dati non validi"),
                             tr("Host e utente non possono contenere spazi o caratteri speciali."));
        return;
    }

    const auto method = static_cast<AuthType>(auth_->currentData().toInt());
    if (method == AuthType::KeyFile && keyPath_->text().trimmed().isEmpty()
        && privateKey_.isEmpty()) {
        QMessageBox::warning(this, tr("Chiave mancante"),
                             tr("Seleziona un file di chiave o importala nel database."));
        return;
    }

    const auto cellText = [this](int row, int column) {
        QTableWidgetItem *item = tunnels_->item(row, column);
        return item ? item->text().trimmed() : QString();
    };
    const auto validPort = [](const QString &text) {
        bool ok = false;
        const uint value = text.toUInt(&ok);
        return ok && value > 0 && value <= 65535;
    };
    for (int row = 0; row < tunnels_->rowCount(); ++row) {
        auto *active = tunnels_->item(row, TunnelEnabledColumn);
        if (active && active->checkState() != Qt::Checked)
            continue;

        const bool localPortOk = validPort(cellText(row, TunnelLocalPortColumn));
        const bool remotePortOk = validPort(cellText(row, TunnelRemotePortColumn));
        QString problem;
        switch (tunnelType(row)) {
        case TunnelType::Local:
            if (!localPortOk || !remotePortOk || cellText(row, TunnelRemoteHostColumn).isEmpty())
                problem = tr("porte e host remoto sono obbligatori");
            break;
        case TunnelType::Remote:
            if (!localPortOk || !remotePortOk)
                problem = tr("porta locale e porta remota sono obbligatorie");
            break;
        case TunnelType::Dynamic:
            if (!localPortOk)
                problem = tr("la porta locale è obbligatoria");
            break;
        }
        if (!problem.isEmpty()) {
            QMessageBox::warning(this, tr("Tunnel non valido"),
                                 tr("Controlla la riga %1 dei tunnel: %2.").arg(row + 1).arg(problem));
            return;
        }
    }

    QDialog::accept();
}
