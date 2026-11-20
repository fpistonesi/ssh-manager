#pragma once

#include "model.h"

#include <QDialog>
#include <QSet>
#include <QString>
#include <QStringList>

class QCheckBox;
class QComboBox;
class QLineEdit;

// Interoperability with the OpenSSH client configuration: ~/.ssh/config in
// both directions, key generation with ssh-keygen and public key files.
namespace openssh {

QString sshDirectory();
QString localUserComment(); // "user@computer", the usual key comment
QString defaultConfigPath();

// An OpenSSH tool (ssh, ssh-keygen...) from the PATH or the Windows
// optional feature folder; empty when missing.
QString toolPath(const QString &name);

// The concrete hosts of a client configuration (wildcard patterns only give
// defaults), with the values ssh would use for each one: the first value
// found wins, "Host *" at the end included. Include directives are followed.
// Returns connections in a store: aliases become names, ProxyJump becomes
// jump hosts (literal hops are added as connections of their own). A host
// already in `current` with the same host, user and port keeps its id, so
// the import can update or skip it.
ConnectionStore importConfig(const QString &path, const ConnectionStore &current,
                             QStringList *warnings);

// "Host" blocks for the given connections. Embedded keys cannot be
// referenced and are noted in comments.
QString exportConfig(const ConnectionStore &store, const QSet<QString> &hostIds);

// The *.pub files of ~/.ssh.
QStringList publicKeyFiles();

// The first line of a public key file if it looks like an OpenSSH public key.
QString readPublicKey(const QString &path, QString *error);

// Remote shell command that appends the key read from standard input to
// ~/.ssh/authorized_keys, unless it is there already.
QString authorizedKeysCommand();

} // namespace openssh

// New key pair with ssh-keygen.
class KeyGenDialog : public QDialog {
    Q_OBJECT

public:
    // `connectionName` (optional) suggests the file name and enables the
    // "use for this connection" options.
    explicit KeyGenDialog(const QString &connectionName, QWidget *parent = nullptr);

    QString privateKeyPath() const { return generated_; }
    QByteArray passphrase() const;
    bool useForConnection() const;
    bool copyToHost() const;
    void accept() override;

private:
    QComboBox *type_ = nullptr;
    QLineEdit *path_ = nullptr;
    QLineEdit *passphrase_ = nullptr;
    QLineEdit *confirm_ = nullptr;
    QLineEdit *comment_ = nullptr;
    QCheckBox *useForConnection_ = nullptr;
    QCheckBox *copyToHost_ = nullptr;
    QString generated_;
};

// Chooses the public key to install on a host.
class PublicKeyDialog : public QDialog {
    Q_OBJECT

public:
    PublicKeyDialog(const QString &target, const QString &suggested, QWidget *parent = nullptr);

    QString path() const;
    QString key() const { return key_; }
    void accept() override;

private:
    QComboBox *file_ = nullptr;
    QString key_;
};
