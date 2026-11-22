// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>

#include "QueueItemDelegate.h"

#include "QueueIcons.h"
#include "QueueTableModel.h"

#include <QPainter>
#include <QPainterPath>

using namespace queue;
using queueicons::Glyph;

namespace
{

struct Tone
{
    QColor fg;
    QColor tint;
};

QColor mix(const QColor &a, const QColor &b, double t)
{
    return QColor::fromRgbF(static_cast<float>(a.redF() * (1 - t) + b.redF() * t),
                            static_cast<float>(a.greenF() * (1 - t) + b.greenF() * t),
                            static_cast<float>(a.blueF() * (1 - t) + b.blueF() * t));
}

QColor hueOf(const QueueEntry &e)
{
    if (e.blocked.isBlocked())
        return QColor(0xc2, 0x41, 0x0c);
    switch (e.lifecycle) {
    case Lifecycle::Posting: return QColor(0x1f, 0x6f, 0xbf);
    case Lifecycle::Preparing: return QColor(0x7a, 0x43, 0xc0);
    case Lifecycle::Prepared: return QColor(0x23, 0x80, 0x4e);
    case Lifecycle::Finalizing: return QColor(0x4f, 0x53, 0xc2);
    case Lifecycle::Quiescing:
    case Lifecycle::Recovering: return QColor(0x8a, 0x6d, 0x00);
    case Lifecycle::Queued: return e.held ? QColor(0x5b, 0x6b, 0x82) : QColor(0x55, 0x5d, 0x6b);
    }
    return QColor(0x55, 0x5d, 0x6b);
}

Tone toneOf(const QueueEntry &e, const QPalette &palette)
{
    const QColor hue = hueOf(e);
    const QColor base = palette.color(QPalette::Base);
    if (palette.color(QPalette::Window).lightness() < 128)
        return { hue.lighter(170), mix(base, hue, 0.30) };
    return { hue.darker(110), mix(base, hue, 0.12) };
}

Glyph glyphOf(const QueueEntry &e)
{
    if (e.blocked.isBlocked())
        return Glyph::Warn;
    switch (e.lifecycle) {
    case Lifecycle::Posting: return Glyph::Play;
    case Lifecycle::Preparing: return Glyph::Gear;
    case Lifecycle::Prepared: return e.held ? Glyph::Pause : Glyph::Box;
    case Lifecycle::Queued: return e.held ? Glyph::Pause : Glyph::Clock;
    default: return Glyph::Lock;
    }
}

bool isBusy(const QueueEntry &e)
{
    return e.lifecycle == Lifecycle::Quiescing || e.lifecycle == Lifecycle::Finalizing
        || e.lifecycle == Lifecycle::Recovering;
}

void paintChip(QPainter *p, const QRect &cell, const QueueEntry &e, const QStyleOptionViewItem &opt)
{
    const Tone tone = toneOf(e, opt.palette);
    QFont font = opt.font;
    font.setWeight(QFont::DemiBold);
    const QFontMetrics fm(font);
    const QString text = QueueTableModel::stateText(e);
    const int h = fm.height() + 4;
    const QRect chip(cell.left() + 6,
                     cell.center().y() - h / 2,
                     12 + 14 + 5 + fm.horizontalAdvance(text) + 10,
                     h);
    p->setPen(QPen(tone.fg, 1));
    p->setBrush(tone.tint);
    p->drawRoundedRect(chip, h / 2.0, h / 2.0);
    queueicons::icon(glyphOf(e), tone.fg, 12)
        .paint(p, QRect(chip.left() + 9, chip.center().y() - 6, 12, 12));
    p->setFont(font);
    p->setPen(tone.fg);
    p->drawText(chip.adjusted(26, 0, -2, 0), Qt::AlignVCenter, text);
}

void paintRank(QPainter *p,
               const QRect &cell,
               const QueueEntry &e,
               int rank,
               const QColor &fg,
               bool selected)
{
    if (e.slot == Slot::Transfer && e.lifecycle == Lifecycle::Posting) {
        queueicons::icon(Glyph::Play, selected ? fg : hueOf(e), 12)
            .paint(p, QRect(cell.left() + 10, cell.center().y() - 6, 12, 12));
    } else if (isBusy(e)) {
        queueicons::icon(Glyph::Lock, fg, 12)
            .paint(p, QRect(cell.left() + 10, cell.center().y() - 6, 12, 12));
    } else {
        p->setPen(Qt::NoPen);
        p->setBrush(mix(fg, QColor(Qt::gray), 0.5));
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 2; ++x)
                p->drawEllipse(QPointF(cell.left() + 13 + x * 4.5,
                                       cell.center().y() - 4.5 + y * 4.5),
                               1.3,
                               1.3);
    }
    p->setPen(fg);
    p->drawText(cell.adjusted(0, 0, -8, 0),
                Qt::AlignVCenter | Qt::AlignRight,
                QString::number(rank));
}

void paintBar(QPainter *p,
              const QRect &cell,
              int percent,
              const QColor &color,
              const QString &text,
              const QColor &fg)
{
    const QRect bar(cell.left() + 6, cell.center().y() - 3, 110, 7);
    p->setPen(Qt::NoPen);
    p->setBrush(mix(fg, QColor(Qt::gray), 0.75));
    p->drawRoundedRect(bar, 3.5, 3.5);
    p->setBrush(color);
    p->drawRoundedRect(
        QRect(bar.left(), bar.top(), bar.width() * std::clamp(percent, 0, 100) / 100, bar.height()),
        3.5,
        3.5);
    p->setPen(fg);
    const QRect textRect(bar.right() + 10,
                         cell.top(),
                         cell.width() - bar.width() - 16,
                         cell.height());
    p->drawText(textRect,
                Qt::AlignVCenter,
                QFontMetrics(p->font()).elidedText(text, Qt::ElideRight, textRect.width()));
}

} // namespace

QueueItemDelegate::QueueItemDelegate(QueueTableModel *model, QObject *parent)
    : QStyledItemDelegate(parent)
    , _model(model)
{
}

QSize QueueItemDelegate::sizeHint(const QStyleOptionViewItem &option,
                                  const QModelIndex &index) const
{
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    size.setHeight(QFontMetrics(option.font).height() + 14);
    return size;
}

void QueueItemDelegate::paint(QPainter *p,
                              const QStyleOptionViewItem &opt,
                              const QModelIndex &index) const
{
    const QueueEntry *e = _model->entryAt(index.row());
    if (!e)
        return;
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const bool selected = opt.state & QStyle::State_Selected;
    const QPalette &pal = opt.palette;
    const QRect cell = opt.rect;
    const bool active = e->slot == Slot::Transfer;
    QColor bg = active ? toneOf(*e, pal).tint : pal.color(QPalette::Base);
    if (selected)
        bg = pal.color(QPalette::Active, QPalette::Highlight);
    p->fillRect(cell, bg);
    if (index.column() == QueueTableModel::RankColumn && (active || e->blocked.isBlocked()))
        p->fillRect(QRect(cell.left(), cell.top(), 4, cell.height()), hueOf(*e));
    const QColor fg = selected ? pal.color(QPalette::Active, QPalette::HighlightedText)
        : e->held              ? pal.color(QPalette::Disabled, QPalette::Text)
                               : pal.color(QPalette::Text);
    const QRect text = cell.adjusted(8, 0, -6, 0);
    p->setPen(fg);

    switch (index.column()) {
    case QueueTableModel::RankColumn: paintRank(p, cell, *e, index.row() + 1, fg, selected); break;
    case QueueTableModel::StateColumn: paintChip(p, cell, *e, opt); break;
    case QueueTableModel::ProgressColumn: {
        const int percent = index.data(QueueTableModel::PercentRole).toInt();
        const QString detail = index.data().toString();
        if (percent >= 0
            && (e->lifecycle == Lifecycle::Posting || e->lifecycle == Lifecycle::Preparing))
            paintBar(p, cell, percent, hueOf(*e), detail, fg);
        else {
            p->setPen(e->blocked.isBlocked() && !selected ? toneOf(*e, pal).fg : fg);
            p->drawText(text,
                        Qt::AlignVCenter,
                        QFontMetrics(opt.font).elidedText(detail, Qt::ElideRight, text.width()));
        }
        break;
    }
    default: {
        QFont font = opt.font;
        if (active && index.column() == QueueTableModel::PostColumn)
            font.setWeight(QFont::DemiBold);
        p->setFont(font);
        const Qt::Alignment align = index.column() == QueueTableModel::SizeColumn ? Qt::AlignRight
                                                                                  : Qt::AlignLeft;
        const Qt::TextElideMode elide = index.column() == QueueTableModel::PostColumn
            ? Qt::ElideMiddle
            : Qt::ElideRight;
        p->drawText(text,
                    Qt::AlignVCenter | align,
                    QFontMetrics(font).elidedText(index.data().toString(), elide, text.width()));
    }
    }
    p->restore();
}
