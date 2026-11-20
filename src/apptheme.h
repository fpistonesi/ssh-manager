#pragma once

#include <QColor>
#include <QString>

// Application look: the Fusion style with a light or dark palette and a
// style sheet modelled on Microsoft 365 (Fluent), applied to the whole
// application at once so that switching never mixes the two.
namespace apptheme {

// "system", "light" or "dark".
void setMode(const QString &mode);
QString mode();

// Re-applies palette and style sheet for the current light/dark state (also
// called when the system theme changes in "system" mode).
void refresh();

bool isDark();

// Status colours readable on the current background.
QColor success();
QColor warning();
QColor danger();
QColor info();
QColor muted();
QColor accent();

} // namespace apptheme
