#include "commandpalette.h"

#include "sshcommand.h"

#include <QAbstractItemView>
#include <QAction>
#include <QKeySequence>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>

namespace {

// Subsequence match: every character of the query must appear in order. Matches
// at word starts and consecutive characters score higher, so "pweb" finds
// "prod-web" before an incidental match in a long path.
int fuzzyScore(const QString &query, const QString &text)
{
    const QString haystack = text.toLower();
    const QStringList tokens = query.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (tokens.isEmpty())
        return 0;

    int total = 0;
    for (const QString &token : tokens) {
        int from = 0;
        int last = -1;
        int score = 0;
        for (const QChar character : token) {
            const int position = haystack.indexOf(character, from);
            if (position < 0)
                return -1;
            score += 10;
            if (position == 0 || !haystack.at(position - 1).isLetterOrNumber())
                score += 12;
            if (last >= 0 && position == last + 1)
                score += 6;
            score -= qMin(position - from, 5);
            last = position;
            from = position + 1;
        }
        total += score;
    }

    if (haystack.contains(query.toLower()))
        total += 50;
    return total;
}

QString searchText(const PaletteEntry &entry)
{
    QString text = entry.title;
    if (entry.kind == PaletteEntry::Kind::Action) {
        if (!entry.shortcut.isEmpty())
            text += QLatin1Char(' ') + entry.shortcut;
    } else if (!entry.detail.isEmpty()) {
        text += QLatin1Char(' ') + entry.detail;
    }
    return text;
}

// Recents and the ad-hoc destination stay above ordinary matches; connections
// stay above actions when scores are close. Favorites get a small extra boost.
int groupBonus(const PaletteEntry &entry)
{
    int bonus = 0;
    switch (entry.kind) {
    case PaletteEntry::Kind::QuickConnect:
        bonus = 4000;
        break;
    case PaletteEntry::Kind::RecentConnection:
        bonus = 3000;
        break;
    case PaletteEntry::Kind::Connection:
        bonus = 2000;
        break;
    case PaletteEntry::Kind::Action:
        bonus = 0;
        break;
    }
    if (entry.favorite)
        bonus += 500;
    return bonus;
}

} // namespace

CommandPalette::CommandPalette(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(tr("Comando rapido"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    resize(680, 460);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    search_ = new QLineEdit(this);
    search_->setPlaceholderText(
        tr("Cerca connessioni e azioni, oppure digita utente@host[:porta]"));
    search_->setClearButtonEnabled(true);
    layout->addWidget(search_);

    list_ = new QListWidget(this);
    list_->setAlternatingRowColors(true);
    list_->setIconSize(QSize(18, 18));
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(list_, 1);

    connect(search_, &QLineEdit::textChanged, this, &CommandPalette::rebuild);
    connect(search_, &QLineEdit::returnPressed, this, &CommandPalette::chooseCurrent);
    connect(list_, &QListWidget::itemActivated, this, &CommandPalette::chooseCurrent);

    search_->setFocus();
}

void CommandPalette::setEntries(const QVector<PaletteEntry> &entries)
{
    entries_ = entries;
    rebuild();
}

void CommandPalette::rebuild()
{
    const QString query = search_->text().trimmed();
    QVector<PaletteEntry> candidates = entries_;

    if (!query.isEmpty()) {
        QString user;
        QString host;
        QString normalized;
        quint16 port = 22;
        if (parseQuickTarget(query, &user, &host, &port, &normalized)) {
            PaletteEntry quick;
            quick.kind = PaletteEntry::Kind::QuickConnect;
            quick.title = normalized;
            quick.detail = tr("Connessione rapida · password o chiave richiesta nel terminale");
            quick.target = normalized;
            quick.icon = QIcon::fromTheme(QStringLiteral("network-server"),
                                          style()->standardIcon(QStyle::SP_DriveNetIcon));
            candidates.prepend(quick);
        }
    }

    struct Scored {
        PaletteEntry entry;
        int score = 0;
    };

    QVector<Scored> scored;
    scored.reserve(candidates.size());
    for (const PaletteEntry &entry : candidates) {
        const int base = query.isEmpty() ? 0 : fuzzyScore(query, searchText(entry));
        if (!query.isEmpty() && base < 0)
            continue;
        scored.append({entry, query.isEmpty() ? 0 : base + groupBonus(entry)});
    }

    if (!query.isEmpty()) {
        std::stable_sort(scored.begin(), scored.end(),
                         [](const Scored &left, const Scored &right) {
                             return left.score > right.score;
                         });
    }

    list_->clear();
    shown_.clear();
    for (const Scored &item : scored) {
        QString text = item.entry.title;
        if (!item.entry.detail.isEmpty())
            text += QStringLiteral("   —   ") + item.entry.detail;
        if (!item.entry.shortcut.isEmpty())
            text += QStringLiteral("   [") + item.entry.shortcut + QLatin1Char(']');

        auto *row = new QListWidgetItem(item.entry.icon, text, list_);
        row->setToolTip(text);
        shown_.append(item.entry);
    }

    if (shown_.isEmpty()) {
        auto *row = new QListWidgetItem(tr("Nessun risultato"), list_);
        row->setFlags(Qt::NoItemFlags);
        list_->setCurrentRow(-1);
    } else {
        list_->setCurrentRow(0);
    }
}

void CommandPalette::chooseCurrent()
{
    if (shown_.isEmpty())
        return;
    accept();
}

PaletteEntry CommandPalette::selectedEntry() const
{
    const int row = list_ ? list_->currentRow() : -1;
    if (row < 0 || row >= shown_.size())
        return {};
    return shown_.at(row);
}
