// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>

#include "QueueIcons.h"

#include <QPainter>
#include <QPixmap>
#include <QPolygonF>

#include <initializer_list>

namespace queueicons
{

namespace
{

//! Drawing context on a 16x16 grid, scaled to the requested size.
struct Pen
{
    QPainter *p = nullptr;
    QColor color;

    void fill(std::initializer_list<QPointF> points) const
    {
        p->save();
        p->setPen(Qt::NoPen);
        p->setBrush(color);
        p->drawPolygon(QPolygonF(points));
        p->restore();
    }
    void line(std::initializer_list<QPointF> points) const { p->drawPolyline(QPolygonF(points)); }
    void rect(const QRectF &r) const { p->fillRect(r, color); }
};

bool drawMedia(Glyph glyph, const Pen &d)
{
    switch (glyph) {
    case Glyph::Play: d.fill({ { 4, 2 }, { 13.5, 8 }, { 4, 14 } }); return true;
    case Glyph::Pause:
        d.rect({ 3, 2, 3.6, 12 });
        d.rect({ 9.4, 2, 3.6, 12 });
        return true;
    case Glyph::Stop: d.rect({ 3, 3, 10, 10 }); return true;
    case Glyph::Next:
        d.fill({ { 2, 2 }, { 10.5, 8 }, { 2, 14 } });
        d.rect({ 11.2, 2, 2.6, 12 });
        return true;
    case Glyph::Up: d.fill({ { 8, 3 }, { 14, 11.5 }, { 2, 11.5 } }); return true;
    case Glyph::Down: d.fill({ { 8, 13 }, { 14, 4.5 }, { 2, 4.5 } }); return true;
    case Glyph::Top:
        d.rect({ 2, 1.5, 12, 2.2 });
        d.fill({ { 8, 5.5 }, { 14, 14 }, { 2, 14 } });
        return true;
    case Glyph::Bottom:
        d.rect({ 2, 12.3, 12, 2.2 });
        d.fill({ { 8, 10.5 }, { 14, 2 }, { 2, 2 } });
        return true;
    default: return false;
    }
}

void drawGear(const Pen &d)
{
    d.p->save();
    d.p->setPen(QPen(d.color, 2.4));
    d.p->drawEllipse(QPointF(8, 8), 3.6, 3.6);
    d.p->setPen(Qt::NoPen);
    d.p->setBrush(d.color);
    for (int i = 0; i < 8; ++i) {
        d.p->save();
        d.p->translate(8, 8);
        d.p->rotate(i * 45);
        d.p->drawRect(QRectF(-1.3, -7.4, 2.6, 2.8));
        d.p->restore();
    }
    d.p->restore();
}

void drawWarn(const Pen &d)
{
    d.fill({ { 8, 1.2 }, { 15.4, 14.6 }, { 0.6, 14.6 } });
    d.p->fillRect(QRectF(7.15, 5.4, 1.7, 5.2), Qt::white);
    d.p->save();
    d.p->setPen(Qt::NoPen);
    d.p->setBrush(Qt::white);
    d.p->drawEllipse(QPointF(8, 12.4), 1.0, 1.0);
    d.p->restore();
}

bool drawStates(Glyph glyph, const Pen &d)
{
    switch (glyph) {
    case Glyph::Gear: drawGear(d); return true;
    case Glyph::Box:
        d.p->drawRect(QRectF(2, 6, 12, 8.5));
        d.line({ { 2, 6 }, { 4, 2.5 }, { 12, 2.5 }, { 14, 6 } });
        d.p->drawLine(QPointF(8, 2.5), QPointF(8, 9));
        return true;
    case Glyph::Clock:
        d.p->drawEllipse(QPointF(8, 8), 6.2, 6.2);
        d.p->drawLine(QPointF(8, 8), QPointF(8, 4.3));
        d.p->drawLine(QPointF(8, 8), QPointF(10.8, 9.6));
        return true;
    case Glyph::Warn: drawWarn(d); return true;
    case Glyph::Lock:
        d.rect({ 3, 7, 10, 8 });
        d.p->drawArc(QRectF(5, 2, 6, 9), 0, 180 * 16);
        return true;
    case Glyph::Resume:
        d.p->drawArc(QRectF(2, 2, 12, 12), 60 * 16, 290 * 16);
        d.fill({ { 12.5, 1.5 }, { 12.8, 5.6 }, { 8.8, 5.2 } });
        return true;
    default: return false;
    }
}

void drawTools(Glyph glyph, const Pen &d)
{
    switch (glyph) {
    case Glyph::Trash:
        d.p->drawLine(QPointF(2, 4), QPointF(14, 4));
        d.p->drawRect(QRectF(6, 1.8, 4, 2.2));
        d.line({ { 3.6, 5.5 }, { 4.6, 15 }, { 11.4, 15 }, { 12.4, 5.5 } });
        break;
    case Glyph::Edit:
        d.fill({ { 2.5, 13.5 }, { 3.5, 10 }, { 11, 2.5 }, { 13.5, 5 }, { 6, 12.5 } });
        break;
    case Glyph::Search:
        d.p->drawEllipse(QPointF(6.8, 6.8), 4.5, 4.5);
        d.p->drawLine(QPointF(10.3, 10.3), QPointF(14, 14));
        break;
    case Glyph::Expand:
        d.p->drawRect(QRectF(2, 2, 12, 12));
        d.rect({ 2, 2, 12, 3 });
        break;
    case Glyph::PopOut:
        d.line({ { 9, 3 }, { 3, 3 }, { 3, 13 }, { 13, 13 }, { 13, 7 } });
        d.p->drawLine(QPointF(7.5, 8.5), QPointF(14, 2));
        d.line({ { 10, 2 }, { 14, 2 }, { 14, 6 } });
        break;
    case Glyph::Chevron: d.line({ { 3, 5.5 }, { 8, 10.5 }, { 13, 5.5 } }); break;
    case Glyph::Plus:
        d.p->drawLine(QPointF(8, 2.5), QPointF(8, 13.5));
        d.p->drawLine(QPointF(2.5, 8), QPointF(13.5, 8));
        break;
    case Glyph::List:
        for (int i = 0; i < 3; ++i) {
            d.rect({ 2, 3 + i * 4.2, 2.4, 2.4 });
            d.rect({ 6, 3.4 + i * 4.2, 8, 1.7 });
        }
        break;
    case Glyph::Folder:
        d.line({ { 1.5, 3 },
                 { 6, 3 },
                 { 7.5, 5 },
                 { 14.5, 5 },
                 { 14.5, 13.5 },
                 { 1.5, 13.5 },
                 { 1.5, 3 } });
        break;
    default: break;
    }
}

} // namespace

QIcon icon(Glyph glyph, const QColor &color, int size)
{
    QPixmap pixmap(size * 2, size * 2);
    pixmap.fill(Qt::transparent);
    {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(size * 2 / 16.0, size * 2 / 16.0);
        painter.setPen(QPen(color, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const Pen pen{ &painter, color };
        if (!drawMedia(glyph, pen) && !drawStates(glyph, pen))
            drawTools(glyph, pen);
    }
    pixmap.setDevicePixelRatio(2.0);
    return QIcon(pixmap);
}

} // namespace queueicons
