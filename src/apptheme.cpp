#include "apptheme.h"

#include <QApplication>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>

namespace apptheme {

namespace {

QString g_mode = QStringLiteral("system");
bool g_dark = false;

// Fluent / Microsoft 365 colour tokens.
struct Tokens {
    QColor window;      // app background around the panels
    QColor surface;     // panels, lists, ribbon card
    QColor surfaceAlt;  // alternate rows
    QColor border;      // subtle separators
    QColor borderStrong; // input borders
    QColor text;
    QColor textMuted;
    QColor textDisabled;
    QColor hover;
    QColor pressed;
    QColor selection;   // selected rows: light tint of the accent
    QColor selectionBorder;
    QColor accent;      // links, focus, selected ribbon tab
    QColor accentFill;  // primary buttons, progress
    QColor onAccent;
    QColor success, warning, danger, info;
};

Tokens tokens(bool dark)
{
    Tokens t;
    if (dark) {
        t.window = QColor(0x1f, 0x1f, 0x1f);
        t.surface = QColor(0x29, 0x29, 0x29);
        t.surfaceAlt = QColor(0x2e, 0x2e, 0x2e);
        t.border = QColor(0x3d, 0x3d, 0x3d);
        t.borderStrong = QColor(0x57, 0x57, 0x57);
        t.text = QColor(0xf5, 0xf5, 0xf5);
        t.textMuted = QColor(0xad, 0xad, 0xad);
        t.textDisabled = QColor(0x6e, 0x6e, 0x6e);
        t.hover = QColor(0x3a, 0x3a, 0x3a);
        t.pressed = QColor(0x33, 0x33, 0x33);
        t.selection = QColor(0x1f, 0x3a, 0x5a);
        t.selectionBorder = QColor(0x2f, 0x6a, 0xa8);
        t.accent = QColor(0x47, 0x9e, 0xf5);
        t.accentFill = QColor(0x11, 0x5e, 0xa3);
        t.onAccent = QColor(0xff, 0xff, 0xff);
        t.success = QColor(0x54, 0xb0, 0x54);
        t.warning = QColor(0xf7, 0xa3, 0x5c);
        t.danger = QColor(0xe3, 0x70, 0x7a);
        t.info = QColor(0x47, 0x9e, 0xf5);
    } else {
        t.window = QColor(0xf5, 0xf5, 0xf5);
        t.surface = QColor(0xff, 0xff, 0xff);
        t.surfaceAlt = QColor(0xfa, 0xfa, 0xfa);
        t.border = QColor(0xe0, 0xe0, 0xe0);
        t.borderStrong = QColor(0xd1, 0xd1, 0xd1);
        t.text = QColor(0x24, 0x24, 0x24);
        t.textMuted = QColor(0x61, 0x61, 0x61);
        t.textDisabled = QColor(0xa6, 0xa6, 0xa6);
        t.hover = QColor(0xf0, 0xf0, 0xf0);
        t.pressed = QColor(0xe0, 0xe0, 0xe0);
        t.selection = QColor(0xeb, 0xf3, 0xfc);
        t.selectionBorder = QColor(0xb4, 0xd6, 0xfa);
        t.accent = QColor(0x0f, 0x6c, 0xbd);
        t.accentFill = QColor(0x0f, 0x6c, 0xbd);
        t.onAccent = QColor(0xff, 0xff, 0xff);
        t.success = QColor(0x10, 0x7c, 0x10);
        t.warning = QColor(0xbc, 0x4b, 0x09);
        t.danger = QColor(0xc5, 0x0f, 0x1f);
        t.info = QColor(0x0f, 0x6c, 0xbd);
    }
    return t;
}

QPalette palette(const Tokens &t)
{
    QPalette p;
    p.setColor(QPalette::Window, t.window);
    p.setColor(QPalette::WindowText, t.text);
    p.setColor(QPalette::Base, t.surface);
    p.setColor(QPalette::AlternateBase, t.surfaceAlt);
    p.setColor(QPalette::Text, t.text);
    p.setColor(QPalette::Button, t.surface);
    p.setColor(QPalette::ButtonText, t.text);
    p.setColor(QPalette::BrightText, t.danger);
    p.setColor(QPalette::ToolTipBase, t.surface);
    p.setColor(QPalette::ToolTipText, t.text);
    p.setColor(QPalette::PlaceholderText, t.textMuted);
    p.setColor(QPalette::Highlight, t.selection);
    p.setColor(QPalette::HighlightedText, t.text);
    p.setColor(QPalette::Link, t.accent);
    p.setColor(QPalette::LinkVisited, t.accent);
    p.setColor(QPalette::Light, t.surface);
    p.setColor(QPalette::Midlight, t.hover);
    p.setColor(QPalette::Mid, t.border);
    p.setColor(QPalette::Dark, t.borderStrong);
    p.setColor(QPalette::Shadow, t.borderStrong);
    p.setColor(QPalette::Accent, t.accent);
    for (QPalette::ColorRole role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, t.textDisabled);
    p.setColor(QPalette::Disabled, QPalette::Highlight, t.hover);
    return p;
}

QString styleSheet(const Tokens &t)
{
    QString css = QStringLiteral(R"(
QMainWindow, QDialog { background: @window; }
QToolTip { background: @surface; color: @text; border: 1px solid @borderStrong; padding: 4px 6px; }

QMenu { background: @surface; color: @text; border: 1px solid @border; padding: 4px; }
QMenu::item { padding: 5px 28px 5px 24px; border-radius: 4px; }
QMenu::item:selected { background: @hover; }
QMenu::item:disabled { color: @textDisabled; }
QMenu::separator { height: 1px; background: @border; margin: 4px 8px; }

QLineEdit, QPlainTextEdit, QTextEdit, QComboBox {
  background: @surface; color: @text; border: 1px solid @borderStrong; border-radius: 4px;
  padding: 3px 6px; selection-background-color: @selectionBorder; selection-color: @text; }
QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QComboBox:focus {
  border: 1px solid @borderStrong; border-bottom: 2px solid @accent; }
QLineEdit:disabled, QComboBox:disabled { color: @textDisabled; background: @window; }
QComboBox QAbstractItemView { background: @surface; border: 1px solid @border; selection-background-color: @selection; selection-color: @text; }

QPushButton { background: @surface; color: @text; border: 1px solid @borderStrong; border-radius: 4px; padding: 5px 14px; }
QPushButton:hover { background: @hover; }
QPushButton:pressed { background: @pressed; }
QPushButton:disabled { color: @textDisabled; border-color: @border; }
QPushButton:default { background: @accentFill; color: @onAccent; border-color: @accentFill; }
QPushButton:default:hover { background: @accent; border-color: @accent; }
QPushButton:default:disabled { background: @hover; color: @textDisabled; border-color: @border; }

QToolButton { border: 1px solid transparent; border-radius: 4px; padding: 2px; }
QToolButton:hover { background: @hover; }
QToolButton:pressed { background: @pressed; }
QToolButton:checked { background: @selection; border-color: @selectionBorder; }

QTreeView, QListView, QTableView {
  background: @surface; alternate-background-color: @surfaceAlt; color: @text;
  border: 1px solid @border; border-radius: 4px; outline: 0;
  selection-background-color: @selection; selection-color: @text; }
QTreeView::item, QListView::item, QTableView::item { padding: 2px 0; }
QTreeView::item:hover, QListView::item:hover, QTableView::item:hover { background: @hover; }
QTreeView::item:selected, QListView::item:selected, QTableView::item:selected { background: @selection; color: @text; }
QHeaderView::section { background: @surface; color: @textMuted; border: 0; border-bottom: 1px solid @border;
  border-right: 1px solid @border; padding: 4px 6px; }

QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle { background: @borderStrong; border-radius: 3px; min-height: 24px; min-width: 24px; }
QScrollBar::handle:hover { background: @textMuted; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QGroupBox { border: 1px solid @border; border-radius: 6px; margin-top: 12px; padding-top: 6px; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: @textMuted; }

QProgressBar { background: @hover; border: 0; border-radius: 3px; text-align: center; color: @text; }
QProgressBar::chunk { background: @accentFill; border-radius: 3px; }

QStatusBar { background: @window; color: @textMuted; border-top: 1px solid @border; }
QSplitter::handle { background: @window; }
QDockWidget { color: @text; }
QDockWidget::title { background: @window; padding: 6px 8px; }

QTabBar::tab { background: transparent; color: @textMuted; border: 0; border-bottom: 2px solid transparent;
  padding: 6px 12px; margin-right: 2px; }
QTabBar::tab:selected { color: @text; border-bottom: 2px solid @accent; }
QTabBar::tab:hover:!selected { background: @hover; border-radius: 4px; }

QTabWidget#ribbon::pane { background: @surface; border: 1px solid @border; border-radius: 6px; top: -1px; }
QTabWidget#ribbon > QStackedWidget { background: @surface; border: 1px solid @border; border-radius: 6px; }
QTabWidget#ribbon > QTabBar::tab { font-weight: 400; padding: 6px 14px; }
QTabWidget#ribbon > QTabBar::tab:selected { color: @accent; border-bottom: 3px solid @accent; font-weight: 600; }
QTabWidget#ribbon QScrollArea, QTabWidget#ribbon QScrollArea > QWidget > QWidget { background: transparent; }
QTabWidget#ribbon QToolButton { background: transparent; color: @text; border: 1px solid transparent; border-radius: 4px; padding: 3px 6px; }
QTabWidget#ribbon QToolButton:hover { background: @hover; }
QTabWidget#ribbon QToolButton:pressed { background: @pressed; }
QTabWidget#ribbon QToolButton:checked { background: @selection; border-color: @selectionBorder; }
QTabWidget#ribbon QToolButton:disabled { color: @textDisabled; }
QTabWidget#ribbon QScrollBar:horizontal { height: 7px; }
QLabel#ribbonGroupCaption { color: @textMuted; }

QMdiArea QTabBar::tab { background: @window; color: @textMuted; border: 1px solid @border; border-bottom: 0;
  border-top-left-radius: 6px; border-top-right-radius: 6px; padding: 5px 10px; margin-right: 2px; }
QMdiArea QTabBar::tab:selected { background: @surface; color: @text; border-top: 2px solid @accent; }
)");
    const QList<QPair<QString, QColor>> values = {
        {QStringLiteral("@window"), t.window},
        {QStringLiteral("@surfaceAlt"), t.surfaceAlt},
        {QStringLiteral("@surface"), t.surface},
        {QStringLiteral("@borderStrong"), t.borderStrong},
        {QStringLiteral("@border"), t.border},
        {QStringLiteral("@textMuted"), t.textMuted},
        {QStringLiteral("@textDisabled"), t.textDisabled},
        {QStringLiteral("@text"), t.text},
        {QStringLiteral("@hover"), t.hover},
        {QStringLiteral("@pressed"), t.pressed},
        {QStringLiteral("@selectionBorder"), t.selectionBorder},
        {QStringLiteral("@selection"), t.selection},
        {QStringLiteral("@accentFill"), t.accentFill},
        {QStringLiteral("@accent"), t.accent},
        {QStringLiteral("@onAccent"), t.onAccent},
    };
    // Longer names first ("@surfaceAlt" before "@surface") by construction.
    for (const auto &value : values)
        css.replace(value.first, value.second.name());
    return css;
}

} // namespace

void setMode(const QString &mode)
{
    g_mode = (mode == QLatin1String("light") || mode == QLatin1String("dark")) ? mode
                                                                               : QStringLiteral("system");
    // The native colour scheme also drives the title bar on Windows 11.
    Qt::ColorScheme scheme = Qt::ColorScheme::Unknown;
    if (g_mode == QLatin1String("light"))
        scheme = Qt::ColorScheme::Light;
    else if (g_mode == QLatin1String("dark"))
        scheme = Qt::ColorScheme::Dark;
    QGuiApplication::styleHints()->setColorScheme(scheme);
    refresh();
}

QString mode()
{
    return g_mode;
}

void refresh()
{
    if (g_mode == QLatin1String("dark"))
        g_dark = true;
    else if (g_mode == QLatin1String("light"))
        g_dark = false;
    else
        g_dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;

    // Fusion follows the palette completely, unlike the native styles.
    if (QApplication::style()->name().compare(QLatin1String("fusion"), Qt::CaseInsensitive) != 0)
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    const Tokens t = tokens(g_dark);
    QApplication::setPalette(palette(t));
    qApp->setStyleSheet(styleSheet(t));
}

bool isDark()
{
    return g_dark;
}

QColor success() { return tokens(g_dark).success; }
QColor warning() { return tokens(g_dark).warning; }
QColor danger() { return tokens(g_dark).danger; }
QColor info() { return tokens(g_dark).info; }
QColor muted() { return tokens(g_dark).textMuted; }
QColor accent() { return tokens(g_dark).accent; }

} // namespace apptheme
