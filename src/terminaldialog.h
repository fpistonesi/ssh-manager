#pragma once

#include "model.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QFontComboBox;
class QLabel;
class QPushButton;
class QSpinBox;

// Modal editor for the terminal profile of a single connection.
class TerminalDialog : public QDialog {
    Q_OBJECT

public:
    explicit TerminalDialog(const TerminalProfile &profile, QWidget *parent = nullptr);

    TerminalProfile profile() const;

private:
    void applyProfileToWidgets(const TerminalProfile &profile);
    void updatePreview();

    QComboBox *colorScheme_ = nullptr;
    QFontComboBox *fontFamily_ = nullptr;
    QSpinBox *fontSize_ = nullptr;
    QSpinBox *opacity_ = nullptr;
    QSpinBox *margin_ = nullptr;
    QComboBox *cursorShape_ = nullptr;
    QCheckBox *blinkCursor_ = nullptr;
    QSpinBox *historySize_ = nullptr;
    QLabel *preview_ = nullptr;
};
