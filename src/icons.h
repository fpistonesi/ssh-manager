#pragma once

#include <QIcon>
#include <QString>

// The application's own line icons (24x24 SVG, Fluent-like). They are drawn
// on demand with the current palette: the text colour for the outline and
// the accent colour for highlights, so they follow the light/dark theme
// without being assigned again.
namespace icons {

QIcon get(const QString &name);

} // namespace icons
