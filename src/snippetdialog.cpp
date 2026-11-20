#include "snippetdialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QVBoxLayout>

SnippetDialog::SnippetDialog(const Snippet &snippet, QWidget *parent)
    : QDialog(parent), snippet_(snippet)
{
    setWindowTitle(snippet.id.isEmpty() ? tr("Nuovo snippet") : tr("Modifica snippet"));
    resize(560, 360);

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout();

    name_ = new QLineEdit(snippet.name, this);
    name_->setPlaceholderText(tr("es. Spazio su disco"));
    form->addRow(tr("Nome"), name_);

    command_ = new QPlainTextEdit(snippet.command, this);
    command_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    command_->setPlaceholderText(tr("es. df -h ${path}"));
    command_->setTabChangesFocus(true);
    form->addRow(tr("Comando"), command_);

    description_ = new QLineEdit(snippet.description, this);
    form->addRow(tr("Descrizione"), description_);

    runImmediately_ = new QCheckBox(tr("Esegui subito (invia anche Invio)"), this);
    runImmediately_->setChecked(snippet.runImmediately);
    form->addRow(QString(), runImmediately_);
    layout->addLayout(form, 1);

    auto *hint = new QLabel(
        tr("Variabili: ${host}, ${user}, ${port} e ${name} vengono dalla sessione di "
           "destinazione; ogni altra ${variabile} viene chiesta all'invio. Più righe vengono "
           "eseguite una dopo l'altra."),
        this);
    hint->setWordWrap(true);
    hint->setEnabled(false);
    layout->addWidget(hint);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &SnippetDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    (snippet.command.isEmpty() ? static_cast<QWidget *>(command_) : name_)->setFocus();
}

void SnippetDialog::accept()
{
    if (command_->toPlainText().trimmed().isEmpty()) {
        QMessageBox::warning(this, windowTitle(), tr("Il comando non può essere vuoto."));
        return;
    }
    QDialog::accept();
}

Snippet SnippetDialog::snippet() const
{
    Snippet result = snippet_;
    result.command = command_->toPlainText().trimmed();
    result.name = name_->text().trimmed();
    if (result.name.isEmpty())
        result.name = result.command.section(QLatin1Char('\n'), 0, 0).left(60);
    result.description = description_->text().trimmed();
    result.runImmediately = runImmediately_->isChecked();
    return result;
}

bool askSnippetVariables(QWidget *parent, const QString &title, const QStringList &names,
                         QHash<QString, QString> *values)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(QObject::tr("Valori per le variabili dello snippet:"), &dialog);
    layout->addWidget(intro);

    auto *form = new QFormLayout();
    QHash<QString, QLineEdit *> edits;
    for (const QString &name : names) {
        auto *edit = new QLineEdit(values->value(name), &dialog);
        edit->setMinimumWidth(280);
        form->addRow(QStringLiteral("${%1}").arg(name), edit);
        edits.insert(name, edit);
    }
    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (!names.isEmpty())
        edits.value(names.first())->setFocus();

    if (dialog.exec() != QDialog::Accepted)
        return false;
    for (auto it = edits.begin(); it != edits.end(); ++it)
        values->insert(it.key(), it.value()->text());
    return true;
}
