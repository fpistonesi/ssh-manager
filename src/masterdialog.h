#pragma once

#include <QByteArray>
#include <QDialog>

class QLabel;
class QLineEdit;
class QProgressBar;

class MasterPasswordDialog : public QDialog {
    Q_OBJECT

public:
    enum class Mode { Create, Unlock, Change };

    explicit MasterPasswordDialog(Mode mode, QWidget *parent = nullptr);

    QByteArray password() const;
    QByteArray currentPassword() const;

    // 0 = empty, 1 = very weak ... 5 = very strong.
    static int passwordScore(const QString &password);

    void setTitleText(const QString &title);
    void setDescriptionText(const QString &description);
    void setError(const QString &message);
    void clearFields();

protected:
    void accept() override;

private:
    void updateStrengthMeter();

    Mode mode_;
    QLineEdit *current_ = nullptr;
    QLineEdit *password_ = nullptr;
    QLineEdit *confirm_ = nullptr;
    QLabel *title_ = nullptr;
    QLabel *description_ = nullptr;
    QLabel *error_ = nullptr;
    QProgressBar *strengthBar_ = nullptr;
    QLabel *strengthText_ = nullptr;
};
