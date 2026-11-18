#include "terminaldialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <qtermwidget.h>

TerminalDialog::TerminalDialog(const TerminalProfile &profile, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Personalizza terminale"));
    setModal(true);
    setMinimumWidth(460);

    auto *layout = new QVBoxLayout(this);

    auto *appearance = new QGroupBox(tr("Aspetto"), this);
    auto *appearanceForm = new QFormLayout(appearance);

    colorScheme_ = new QComboBox(appearance);
    for (const QString &scheme : QTermWidget::availableColorSchemes())
        colorScheme_->addItem(scheme, scheme);

    fontFamily_ = new QFontComboBox(appearance);
    fontSize_ = new QSpinBox(appearance);
    fontSize_->setRange(6, 48);
    fontSize_->setSuffix(tr(" pt"));

    opacity_ = new QSpinBox(appearance);
    opacity_->setRange(20, 100);
    opacity_->setSuffix(QStringLiteral(" %"));
    opacity_->setToolTip(tr("Richiede un compositore con trasparenza attiva."));

    margin_ = new QSpinBox(appearance);
    margin_->setRange(0, 24);
    margin_->setSuffix(tr(" px"));

    appearanceForm->addRow(tr("Schema colori"), colorScheme_);
    appearanceForm->addRow(tr("Font"), fontFamily_);
    appearanceForm->addRow(tr("Dimensione"), fontSize_);
    appearanceForm->addRow(tr("Opacità"), opacity_);
    appearanceForm->addRow(tr("Margine"), margin_);
    layout->addWidget(appearance);

    auto *behaviour = new QGroupBox(tr("Cursore e cronologia"), this);
    auto *behaviourForm = new QFormLayout(behaviour);

    cursorShape_ = new QComboBox(behaviour);
    cursorShape_->addItem(tr("Blocco"), 0);
    cursorShape_->addItem(tr("Sottolineato"), 1);
    cursorShape_->addItem(tr("Barra"), 2);

    blinkCursor_ = new QCheckBox(tr("Cursore lampeggiante"), behaviour);

    historySize_ = new QSpinBox(behaviour);
    historySize_->setRange(100, 200000);
    historySize_->setSingleStep(1000);
    historySize_->setSuffix(tr(" righe"));

    behaviourForm->addRow(tr("Forma"), cursorShape_);
    behaviourForm->addRow(QString(), blinkCursor_);
    behaviourForm->addRow(tr("Cronologia"), historySize_);
    layout->addWidget(behaviour);

    preview_ = new QLabel(this);
    preview_->setObjectName(QStringLiteral("terminalPreview"));
    preview_->setText(QStringLiteral("user@host:~$ ls -la"));
    preview_->setAlignment(Qt::AlignCenter);
    preview_->setMinimumHeight(44);
    preview_->setAutoFillBackground(true);
    preview_->setStyleSheet(
        QStringLiteral("QLabel#terminalPreview { border: 1px solid palette(mid);"
                       " border-radius: 4px; padding: 4px; }"));
    layout->addWidget(preview_);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Reset, this);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Reset), &QPushButton::clicked, this,
            [this] { applyProfileToWidgets(TerminalProfile{}); });
    connect(fontFamily_, &QFontComboBox::currentFontChanged, this,
            [this] { updatePreview(); });
    connect(fontSize_, &QSpinBox::valueChanged, this, [this] { updatePreview(); });

    applyProfileToWidgets(profile);
}

void TerminalDialog::applyProfileToWidgets(const TerminalProfile &profile)
{
    // Keep custom schemes that are not in the shipped list.
    int schemeIndex = colorScheme_->findData(profile.colorScheme);
    if (schemeIndex < 0 && !profile.colorScheme.isEmpty()) {
        colorScheme_->addItem(profile.colorScheme, profile.colorScheme);
        schemeIndex = colorScheme_->count() - 1;
    }
    colorScheme_->setCurrentIndex(qMax(0, schemeIndex));

    QFont font = profile.fontFamily.isEmpty()
                     ? QFontDatabase::systemFont(QFontDatabase::FixedFont)
                     : QFont(profile.fontFamily);
    fontFamily_->setCurrentFont(font);
    fontSize_->setValue(qBound(6, profile.fontSize, 48));

    opacity_->setValue(qBound(20, profile.opacityPercent, 100));
    margin_->setValue(qBound(0, profile.margin, 24));

    const int cursorIndex = cursorShape_->findData(qBound(0, profile.cursorShape, 2));
    cursorShape_->setCurrentIndex(qMax(0, cursorIndex));
    blinkCursor_->setChecked(profile.blinkCursor);
    historySize_->setValue(qBound(100, profile.historySize, 200000));

    updatePreview();
}

TerminalProfile TerminalDialog::profile() const
{
    TerminalProfile profile;
    profile.colorScheme = colorScheme_->currentData().toString();
    profile.fontFamily = fontFamily_->currentFont().family();
    profile.fontSize = fontSize_->value();
    profile.opacityPercent = opacity_->value();
    profile.margin = margin_->value();
    profile.cursorShape = cursorShape_->currentData().toInt();
    profile.blinkCursor = blinkCursor_->isChecked();
    profile.historySize = historySize_->value();
    return profile;
}

void TerminalDialog::updatePreview()
{
    QFont font = fontFamily_->currentFont();
    font.setPointSize(fontSize_->value());
    preview_->setFont(font);
}
