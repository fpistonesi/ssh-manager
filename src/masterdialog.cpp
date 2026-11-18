#include "masterdialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>

#include <cmath>

MasterPasswordDialog::MasterPasswordDialog(Mode mode, QWidget *parent)
    : QDialog(parent), mode_(mode)
{
    setMinimumWidth(430);
    setModal(true);

    auto *layout = new QVBoxLayout(this);

    title_ = new QLabel(this);
    QFont titleFont = title_->font();
    titleFont.setBold(true);
    title_->setFont(titleFont);
    layout->addWidget(title_);

    description_ = new QLabel(this);
    description_->setWordWrap(true);
    layout->addWidget(description_);

    auto *form = new QFormLayout();
    if (mode_ == Mode::Change) {
        current_ = new QLineEdit(this);
        current_->setEchoMode(QLineEdit::Password);
        form->addRow(tr("Password attuale"), current_);
    }
    password_ = new QLineEdit(this);
    password_->setEchoMode(QLineEdit::Password);
    // Only create the confirmation field when it is actually added to the
    // layout; otherwise it would float unmanaged in the dialog's top-left.
    if (mode_ != Mode::Unlock) {
        confirm_ = new QLineEdit(this);
        confirm_->setEchoMode(QLineEdit::Password);
    }

    if (mode_ == Mode::Unlock) {
        form->addRow(tr("Master password"), password_);
    } else if (mode_ == Mode::Create) {
        form->addRow(tr("Master password"), password_);
        form->addRow(tr("Conferma"), confirm_);
    } else {
        form->addRow(tr("Nuova password"), password_);
        form->addRow(tr("Conferma nuova password"), confirm_);
    }
    layout->addLayout(form);

    if (mode_ != Mode::Unlock) {
        auto *strengthRow = new QHBoxLayout();
        strengthBar_ = new QProgressBar(this);
        strengthBar_->setRange(0, 5);
        strengthBar_->setValue(0);
        strengthBar_->setTextVisible(false);
        strengthBar_->setFixedHeight(10);
        strengthText_ = new QLabel(this);
        strengthText_->setMinimumWidth(140);
        strengthRow->addWidget(strengthBar_, 1);
        strengthRow->addWidget(strengthText_);
        layout->addLayout(strengthRow);

        connect(password_, &QLineEdit::textChanged, this,
                [this] { updateStrengthMeter(); });
        updateStrengthMeter();
    }

    error_ = new QLabel(this);
    error_->setWordWrap(true);
    error_->setStyleSheet(QStringLiteral("color: #c0392b;"));
    error_->hide();
    layout->addWidget(error_);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &MasterPasswordDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    switch (mode_) {
    case Mode::Create:
        setWindowTitle(tr("Crea archivio cifrato"));
        title_->setText(tr("Crea la master password"));
        description_->setText(tr("Connessioni e chiavi private saranno cifrate in un unico archivio. "
                                 "La master password non può essere recuperata: se la perdi, i dati "
                                 "non sono più accessibili."));
        break;
    case Mode::Unlock:
        setWindowTitle(tr("Sblocca archivio"));
        title_->setText(tr("Archivio bloccato"));
        description_->setText(tr("Inserisci la master password per accedere alle connessioni e alle "
                                 "chiavi salvate."));
        break;
    case Mode::Change:
        setWindowTitle(tr("Cambia master password"));
        title_->setText(tr("Cambia master password"));
        description_->setText(tr("L'archivio verrà ricifrato con la nuova password."));
        break;
    }

    password_->setFocus();
}

QByteArray MasterPasswordDialog::password() const
{
    return password_->text().toUtf8();
}

QByteArray MasterPasswordDialog::currentPassword() const
{
    return current_ ? current_->text().toUtf8() : QByteArray();
}

void MasterPasswordDialog::setTitleText(const QString &title)
{
    title_->setText(title);
}

void MasterPasswordDialog::setDescriptionText(const QString &description)
{
    description_->setText(description);
}

int MasterPasswordDialog::passwordScore(const QString &password)
{
    if (password.isEmpty())
        return 0;

    const QString lower = password.toLower();
    static const QStringList weak = {
        QStringLiteral("password"),    QStringLiteral("password1"), QStringLiteral("12345678"),
        QStringLiteral("123456789"),   QStringLiteral("qwerty"),    QStringLiteral("qwertyui"),
        QStringLiteral("abc123"),      QStringLiteral("admin"),     QStringLiteral("letmein"),
        QStringLiteral("welcome"),     QStringLiteral("iloveyou"),  QStringLiteral("monkey"),
        QStringLiteral("dragon"),      QStringLiteral("master"),    QStringLiteral("sshmanager"),
    };
    for (const QString &candidate : weak) {
        if (lower == candidate || lower.startsWith(candidate + QLatin1Char('0'))
            || lower.startsWith(candidate + QLatin1Char('1')))
            return 1;
    }

    bool hasLower = false;
    bool hasUpper = false;
    bool hasDigit = false;
    bool hasSymbol = false;
    for (const QChar ch : password) {
        if (ch.isLower())
            hasLower = true;
        else if (ch.isUpper())
            hasUpper = true;
        else if (ch.isDigit())
            hasDigit = true;
        else
            hasSymbol = true;
    }
    const int alphabet = (hasLower ? 26 : 0) + (hasUpper ? 26 : 0) + (hasDigit ? 10 : 0)
                         + (hasSymbol ? 33 : 0);

    bool repeated = password.size() >= 6;
    for (int i = 1; repeated && i < password.size(); ++i) {
        if (password.at(i) != password.at(0))
            repeated = false;
    }
    if (repeated)
        return 1;

    // Rough entropy estimate: length * log2(alphabet size).
    const double bits = password.size() * std::log2(double(qMax(alphabet, 1)));
    if (bits < 30)
        return 1;
    if (bits < 40)
        return 2;
    if (bits < 60)
        return 3;
    if (bits < 80)
        return 4;
    return 5;
}

void MasterPasswordDialog::updateStrengthMeter()
{
    if (!strengthBar_ || !strengthText_)
        return;

    const int score = passwordScore(password_->text());
    QString label;
    QString color;
    switch (score) {
    case 1:
        label = tr("Molto debole");
        color = QStringLiteral("#c0392b");
        break;
    case 2:
        label = tr("Debole");
        color = QStringLiteral("#e67e22");
        break;
    case 3:
        label = tr("Media");
        color = QStringLiteral("#f1c40f");
        break;
    case 4:
        label = tr("Forte");
        color = QStringLiteral("#2ecc71");
        break;
    case 5:
        label = tr("Molto forte");
        color = QStringLiteral("#27ae60");
        break;
    default:
        label = QStringLiteral("\u2014");
        color = QStringLiteral("#bdc3c7");
        break;
    }

    strengthBar_->setValue(score);
    strengthBar_->setStyleSheet(QStringLiteral("QProgressBar::chunk { background-color: %1; }")
                                    .arg(color));
    strengthText_->setText(tr("Robustezza: %1").arg(label));
    strengthText_->setStyleSheet(QStringLiteral("color: %1;").arg(color));
}

void MasterPasswordDialog::setError(const QString &message)
{
    error_->setText(message);
    error_->setVisible(!message.isEmpty());
}

void MasterPasswordDialog::clearFields()
{
    if (current_)
        current_->clear();
    password_->clear();
    if (confirm_)
        confirm_->clear();
    password_->setFocus();
}

void MasterPasswordDialog::accept()
{
    if (mode_ != Mode::Unlock && password_->text().size() < 8) {
        setError(tr("La master password deve contenere almeno 8 caratteri."));
        return;
    }
    if (mode_ == Mode::Unlock && password_->text().isEmpty()) {
        setError(tr("Inserisci la master password."));
        return;
    }
    if (mode_ != Mode::Unlock && password_->text() != confirm_->text()) {
        setError(tr("Le due password non coincidono."));
        return;
    }
    if (mode_ == Mode::Change && current_->text().isEmpty()) {
        setError(tr("Inserisci la password attuale."));
        return;
    }

    setError(QString());
    QDialog::accept();
}
