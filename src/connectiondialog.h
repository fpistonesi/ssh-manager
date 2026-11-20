#pragma once

#include "model.h"

#include <QDialog>
#include <QVector>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

class ConnectionDialog : public QDialog {
    Q_OBJECT

public:
    explicit ConnectionDialog(const ConnectionStore &store, QWidget *parent = nullptr);
    ~ConnectionDialog() override;

    void setConnection(const Connection &connection);
    void setFolderId(const QString &folderId);
    Connection connection() const;
    QString folderId() const;

protected:
    void accept() override;

private:
    enum TunnelColumn {
        TunnelEnabledColumn,
        TunnelTypeColumn,
        TunnelLocalAddressColumn,
        TunnelLocalPortColumn,
        TunnelRemoteHostColumn,
        TunnelRemotePortColumn,
        TunnelColumnCount
    };

    void populateFolders();
    void populateJumpHosts(const QString &excludeId, const QString &keepId = QString());
    void updateAuthRows();
    void updateKeyStatus();
    void updateTerminalSummary();
    void addTunnelRow(const TunnelConfig &tunnel);
    QVector<TunnelConfig> readTunnels() const;
    TunnelType tunnelType(int row) const;
    void updateTunnelRow(int row);

    const ConnectionStore &store_;
    QString connectionId_;

    QLineEdit *name_ = nullptr;
    QComboBox *folder_ = nullptr;
    QCheckBox *favorite_ = nullptr;
    QCheckBox *jumpHost_ = nullptr;
    QLineEdit *tags_ = nullptr;
    QLineEdit *host_ = nullptr;
    QSpinBox *port_ = nullptr;
    QLineEdit *user_ = nullptr;

    QComboBox *auth_ = nullptr;
    QFormLayout *authForm_ = nullptr;
    QLineEdit *password_ = nullptr;
    QCheckBox *showPassword_ = nullptr;
    QWidget *passwordRow_ = nullptr;
    QLineEdit *keyPath_ = nullptr;
    QWidget *keyRow_ = nullptr;
    QLabel *keyStatus_ = nullptr;
    QPushButton *removeKeyButton_ = nullptr;
    QWidget *keyStatusRow_ = nullptr;
    QLineEdit *passphrase_ = nullptr;
    QWidget *agentRow_ = nullptr;

    QByteArray privateKey_;
    QString keyName_;
    TerminalProfile terminal_;
    QLabel *terminalSummary_ = nullptr;

    QComboBox *jump_ = nullptr;
    QTableWidget *tunnels_ = nullptr;
    QPlainTextEdit *notes_ = nullptr;
    QLineEdit *startupCommand_ = nullptr;
};
