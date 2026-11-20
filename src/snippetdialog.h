#pragma once

#include "model.h"

#include <QDialog>
#include <QHash>

class QCheckBox;
class QLineEdit;
class QPlainTextEdit;

// Creates or edits a snippet.
class SnippetDialog : public QDialog {
    Q_OBJECT

public:
    explicit SnippetDialog(const Snippet &snippet, QWidget *parent = nullptr);
    Snippet snippet() const;

protected:
    void accept() override;

private:
    Snippet snippet_;
    QLineEdit *name_ = nullptr;
    QPlainTextEdit *command_ = nullptr;
    QLineEdit *description_ = nullptr;
    QCheckBox *runImmediately_ = nullptr;
};

// Asks the values of a snippet's custom ${variables}. `values` holds the
// previous answers on input and the new ones on output.
bool askSnippetVariables(QWidget *parent, const QString &title, const QStringList &names,
                         QHash<QString, QString> *values);
