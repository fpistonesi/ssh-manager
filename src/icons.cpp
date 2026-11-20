#include "icons.h"

#include "apptheme.h"

#include <QApplication>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QSvgRenderer>

namespace icons {

namespace {

// Bodies of 24x24 outline icons. "FG" is the text colour, "AC" the accent.
const QHash<QString, QString> &bodies()
{
    static const QHash<QString, QString> map = {
        // Connections
        {QStringLiteral("connect"),
         QStringLiteral(R"(<rect x="3" y="4" width="18" height="16" rx="2.5"/><path stroke="AC" d="M7.5 9l3 3-3 3M13 15h3.5"/>)")},
        {QStringLiteral("local-terminal"),
         QStringLiteral(R"(<rect x="3" y="4" width="18" height="12" rx="2"/><path d="M8.5 20h7M12 16v4"/><path stroke="AC" d="M7 8l2.5 2L7 12"/>)")},
        {QStringLiteral("search"),
         QStringLiteral(R"(<circle cx="10.5" cy="10.5" r="6"/><path d="M15 15l5.5 5.5"/>)")},
        {QStringLiteral("transfer"),
         QStringLiteral(R"(<path d="M8 19.5V5M4.5 8.5L8 5l3.5 3.5"/><path stroke="AC" d="M16 4.5V19M12.5 15.5L16 19l3.5-3.5"/>)")},
        {QStringLiteral("tunnel"),
         QStringLiteral(R"(<path d="M3.5 19.5V12a8.5 8.5 0 0 1 17 0v7.5M2 19.5h20"/><path stroke="AC" d="M8 19.5V13a4 4 0 0 1 8 0v6.5"/>)")},
        {QStringLiteral("batch"),
         QStringLiteral(R"(<rect x="3" y="4" width="11" height="6.5" rx="1.5"/><rect x="3" y="13.5" width="11" height="6.5" rx="1.5"/><path d="M6 7.25h.01M6 16.75h.01"/><path stroke="AC" d="M17.5 8.5l4 3.5-4 3.5z"/>)")},
        {QStringLiteral("health"),
         QStringLiteral(R"(<path stroke="AC" d="M3 12.5h3.5l2.5-6 4.5 12 3-7.5H21"/>)")},
        {QStringLiteral("reachability"),
         QStringLiteral(R"(<path d="M2.5 9.5a13.5 13.5 0 0 1 19 0M5.5 12.8a9 9 0 0 1 13 0M8.5 16a4.6 4.6 0 0 1 7 0"/><circle stroke="AC" cx="12" cy="19" r="1"/>)")},
        {QStringLiteral("refresh"),
         QStringLiteral(R"(<path d="M19.5 11A7.6 7.6 0 0 0 6 7.2L4.5 8.8M4.5 4.5v4.3h4.3M4.5 13A7.6 7.6 0 0 0 18 16.8l1.5-1.6M19.5 19.5v-4.3h-4.3"/>)")},
        {QStringLiteral("server-add"),
         QStringLiteral(R"(<rect x="3" y="4" width="14" height="6.5" rx="1.5"/><path d="M15 13.5H4.5A1.5 1.5 0 0 0 3 15v3.5A1.5 1.5 0 0 0 4.5 20H13M6 7.25h.01M6 16.75h.01"/><path stroke="AC" d="M19 13.5v7M15.5 17h7"/>)")},
        {QStringLiteral("folder-add"),
         QStringLiteral(R"(<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/><path stroke="AC" d="M12 10v6M9 13h6"/>)")},
        {QStringLiteral("edit"),
         QStringLiteral(R"(<path d="M4 20h4L19 9a2.8 2.8 0 0 0-4-4L4 16z"/><path stroke="AC" d="M13.5 6.5l4 4"/>)")},
        {QStringLiteral("copy"),
         QStringLiteral(R"(<rect x="8" y="8" width="12" height="12" rx="2"/><path d="M16 8V6a2 2 0 0 0-2-2H6a2 2 0 0 0-2 2v8a2 2 0 0 0 2 2h2"/>)")},
        {QStringLiteral("rename"),
         QStringLiteral(R"(<path d="M15 7H5a2 2 0 0 0-2 2v6a2 2 0 0 0 2 2h10M7 12h4"/><path stroke="AC" d="M19 4.5v15M17 4.5h4M17 19.5h4"/>)")},
        {QStringLiteral("star"),
         QStringLiteral(R"(<path d="M12 3.5l2.6 5.3 5.9.9-4.3 4.1 1 5.8L12 16.9l-5.2 2.7 1-5.8-4.3-4.1 5.9-.9z"/>)")},
        {QStringLiteral("delete"),
         QStringLiteral(R"(<path d="M4 7h16M6.5 7l.9 12.1A2 2 0 0 0 9.4 21h5.2a2 2 0 0 0 2-1.9L17.5 7M9 7V5a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v2M10 11v6M14 11v6"/>)")},
        // Sessions
        {QStringLiteral("broadcast"),
         QStringLiteral(R"(<circle stroke="AC" cx="12" cy="12" r="2"/><path d="M8.5 15.5a5 5 0 0 1 0-7M15.5 8.5a5 5 0 0 1 0 7M5.6 18.4a9 9 0 0 1 0-12.8M18.4 5.6a9 9 0 0 1 0 12.8"/>)")},
        {QStringLiteral("include"),
         QStringLiteral(R"(<rect x="4" y="4" width="16" height="16" rx="3"/><path stroke="AC" d="M8 12.5l3 3 5-6"/>)")},
        {QStringLiteral("terminal-settings"),
         QStringLiteral(R"(<path d="M4 6.5h9M19 6.5h1M4 12h3M11 12h9M4 17.5h11M19 17.5h1"/><circle stroke="AC" cx="16" cy="6.5" r="2"/><circle stroke="AC" cx="9" cy="12" r="2"/><circle stroke="AC" cx="17" cy="17.5" r="2"/>)")},
        {QStringLiteral("snippet"),
         QStringLiteral(R"(<path d="M8 4H7a2 2 0 0 0-2 2v4l-2 2 2 2v4a2 2 0 0 0 2 2h1M16 4h1a2 2 0 0 1 2 2v4l2 2-2 2v4a2 2 0 0 1-2 2h-1"/><path stroke="AC" d="M10 12h.01M14 12h.01"/>)")},
        {QStringLiteral("snippet-add"),
         QStringLiteral(R"(<path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8zM14 3v5h5"/><path stroke="AC" d="M12 11v6M9 14h6"/>)")},
        {QStringLiteral("history"),
         QStringLiteral(R"(<path d="M3.5 12a8.5 8.5 0 1 0 2.6-6.1L3.5 8.5M3.5 4v4.5H8"/><path stroke="AC" d="M12 7.5V12l3 2"/>)")},
        {QStringLiteral("previous"), QStringLiteral(R"(<path d="M15 5l-7 7 7 7"/>)")},
        {QStringLiteral("next"), QStringLiteral(R"(<path d="M9 5l7 7-7 7"/>)")},
        {QStringLiteral("close"), QStringLiteral(R"(<path d="M6 6l12 12M18 6L6 18"/>)")},
        {QStringLiteral("close-all"),
         QStringLiteral(R"(<rect x="7.5" y="7.5" width="13" height="13" rx="2"/><path d="M3.5 16.5v-11a2 2 0 0 1 2-2h11M11.5 11.5l5 5M16.5 11.5l-5 5"/>)")},
        // View
        {QStringLiteral("maximize"),
         QStringLiteral(R"(<path d="M4 9V4h5M20 9V4h-5M4 15v5h5M20 15v5h-5"/>)")},
        {QStringLiteral("restore"),
         QStringLiteral(R"(<path d="M9 4v5H4M15 4v5h5M9 20v-5H4M15 20v-5h5"/>)")},
        {QStringLiteral("panel-tree"),
         QStringLiteral(R"(<rect x="3" y="3" width="6" height="4.5" rx="1"/><path d="M6 7.5v11h5M6 12.5h5"/><rect stroke="AC" x="11" y="10.25" width="10" height="4.5" rx="1"/><rect stroke="AC" x="11" y="16.25" width="10" height="4.5" rx="1"/>)")},
        {QStringLiteral("panel-list"),
         QStringLiteral(R"(<path d="M9 6h11M9 12h11M9 18h11"/><path stroke="AC" d="M4.5 6h.01M4.5 12h.01M4.5 18h.01"/>)")},
        {QStringLiteral("reset-layout"),
         QStringLiteral(R"(<path d="M4 12a8 8 0 1 0 2.4-5.7L4 8.7M4 4v4.7h4.7"/>)")},
        {QStringLiteral("theme"),
         QStringLiteral(R"(<circle cx="12" cy="12" r="8.5"/><path fill="FG" stroke="none" d="M12 3.5a8.5 8.5 0 0 1 0 17z"/>)")},
        {QStringLiteral("language"),
         QStringLiteral(R"(<circle cx="12" cy="12" r="8.5"/><path d="M3.5 12h17M12 3.5c2.6 2.3 3.8 5.2 3.8 8.5s-1.2 6.2-3.8 8.5c-2.6-2.3-3.8-5.2-3.8-8.5s1.2-6.2 3.8-8.5z"/>)")},
        {QStringLiteral("filter"),
         QStringLiteral(R"(<path d="M4 5h16l-6.2 7.3V18l-3.6 2v-7.7z"/>)")},
        {QStringLiteral("tools"),
         QStringLiteral(R"(<path d="M14.7 6.3a1 1 0 0 0 0 1.4l1.6 1.6a1 1 0 0 0 1.4 0l3.8-3.8a6 6 0 0 1-7.9 7.9l-6.9 6.9a2.1 2.1 0 0 1-3-3l6.9-6.9a6 6 0 0 1 7.9-7.9z"/>)")},
        // Archive
        {QStringLiteral("save"),
         QStringLiteral(R"(<path d="M6 3.5h10l4 4V18.5a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2v-13a2 2 0 0 1 2-2z"/><path d="M8 3.5V8h7V3.5"/><path stroke="AC" d="M8 20.5V14h8v6.5"/>)")},
        {QStringLiteral("export"),
         QStringLiteral(R"(<path d="M4 15v3.5a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V15"/><path stroke="AC" d="M12 15V3.5M7.5 8L12 3.5 16.5 8"/>)")},
        {QStringLiteral("import"),
         QStringLiteral(R"(<path d="M4 15v3.5a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V15"/><path stroke="AC" d="M12 3.5V15M7.5 10.5L12 15l4.5-4.5"/>)")},
        {QStringLiteral("backup"),
         QStringLiteral(R"(<rect x="3" y="4" width="18" height="5" rx="1.5"/><path d="M5 9v9a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V9"/><path stroke="AC" d="M10 13h4"/>)")},
        {QStringLiteral("file-import"),
         QStringLiteral(R"(<path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8zM14 3v5h5"/><path stroke="AC" d="M12 10.5v6.5M9 14l3 3 3-3"/>)")},
        {QStringLiteral("file-export"),
         QStringLiteral(R"(<path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8zM14 3v5h5"/><path stroke="AC" d="M12 17.5V11M9 14l3-3 3 3"/>)")},
        {QStringLiteral("key-add"),
         QStringLiteral(R"(<circle cx="7.5" cy="16.5" r="3.5"/><path d="M10 14l6-6M13.5 10.5l2 2"/><path stroke="AC" d="M18.5 3v6M15.5 6h6"/>)")},
        {QStringLiteral("key-send"),
         QStringLiteral(R"(<circle cx="6.5" cy="14" r="3.5"/><path d="M10 14h5.5M13 14v2.5"/><path stroke="AC" d="M19 18.5V5M15.5 8.5L19 5l3.5 3.5"/>)")},
        {QStringLiteral("lock"),
         QStringLiteral(R"(<rect x="5" y="10.5" width="14" height="10" rx="2"/><path d="M8 10.5V7a4 4 0 0 1 8 0v3.5"/><path stroke="AC" d="M12 14.5v2"/>)")},
        {QStringLiteral("unlock"),
         QStringLiteral(R"(<rect x="5" y="10.5" width="14" height="10" rx="2"/><path d="M8 10.5V7a4 4 0 0 1 7.7-1.5"/><path stroke="AC" d="M12 14.5v2"/>)")},
        {QStringLiteral("password"),
         QStringLiteral(R"(<rect x="2.5" y="6" width="19" height="12" rx="2.5"/><path stroke="AC" d="M7 10v4M5.3 11l3.4 2M5.3 13l3.4-2M12 10v4M10.3 11l3.4 2M10.3 13l3.4-2M16 14h3"/>)")},
        {QStringLiteral("timer"),
         QStringLiteral(R"(<circle cx="12" cy="13.5" r="7.5"/><path d="M10 2.5h4"/><path stroke="AC" d="M12 9.5v4l2.5 1.5"/>)")},
        {QStringLiteral("host-keys"),
         QStringLiteral(R"(<path d="M12 3l7 3v5c0 4.5-3 8.5-7 10-4-1.5-7-5.5-7-10V6z"/><path stroke="AC" d="M9 12l2 2 4-4"/>)")},
        {QStringLiteral("info"),
         QStringLiteral(R"(<circle cx="12" cy="12" r="8.5"/><path stroke="AC" d="M12 11v5.5M12 7.5h.01"/>)")},
    };
    return map;
}

class SvgIconEngine : public QIconEngine {
public:
    explicit SvgIconEngine(const QString &body) : body_(body) {}

    QIconEngine *clone() const override { return new SvgIconEngine(body_); }

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State) override
    {
        QSvgRenderer renderer(svg(mode));
        renderer.render(painter, rect);
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State, qreal scale) override
    {
        QPixmap pixmap(size * scale);
        pixmap.fill(Qt::transparent);
        {
            QPainter painter(&pixmap);
            QSvgRenderer renderer(svg(mode));
            renderer.render(&painter, QRectF(QPointF(0, 0), QSizeF(size * scale)));
        }
        pixmap.setDevicePixelRatio(scale);
        return pixmap;
    }

    QString key() const override { return QStringLiteral("sshmanager-svg"); }

private:
    QByteArray svg(QIcon::Mode mode) const
    {
        const QPalette palette = QApplication::palette();
        QColor foreground = palette.color(QPalette::WindowText);
        QColor accent = apptheme::accent();
        if (mode == QIcon::Disabled) {
            foreground = palette.color(QPalette::Disabled, QPalette::WindowText);
            accent = foreground;
        } else if (mode == QIcon::Selected) {
            foreground = palette.color(QPalette::HighlightedText);
            accent = foreground;
        }
        QString text = QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" "
                                      "fill=\"none\" stroke=\"FG\" stroke-width=\"1.6\" "
                                      "stroke-linecap=\"round\" stroke-linejoin=\"round\">")
                       + body_ + QStringLiteral("</svg>");
        text.replace(QLatin1String("\"FG\""), QLatin1Char('"') + foreground.name() + QLatin1Char('"'));
        text.replace(QLatin1String("\"AC\""), QLatin1Char('"') + accent.name() + QLatin1Char('"'));
        return text.toUtf8();
    }

    QString body_;
};

} // namespace

QIcon get(const QString &name)
{
    const auto it = bodies().constFind(name);
    if (it == bodies().cend())
        return QIcon();
    return QIcon(new SvgIconEngine(it.value()));
}

} // namespace icons
