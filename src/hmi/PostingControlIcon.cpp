//========================================================================
//
// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3..
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>
//
//========================================================================

#include "PostingControlIcon.h"

#include <QPainter>
#include <QPixmap>
#include <QPolygonF>

namespace PostingControlIcon
{

namespace
{

// Large enough to scale down cleanly at any zoom and device pixel ratio;
// the drawing itself is laid out on a 100 x 100 grid.
constexpr int kPixmapSize = 256;
constexpr qreal kGrid = 100.0;
constexpr qreal kRimWidth = 5.5;

void drawGlyph(QPainter &painter, Glyph glyph)
{
    switch (glyph) {
    case Glyph::NewTab:
        painter.drawRoundedRect(QRectF(23, 43, 54, 14), 2.8, 2.8);
        painter.drawRoundedRect(QRectF(43, 23, 14, 54), 2.8, 2.8);
        break;
    case Glyph::Pause:
        painter.drawRoundedRect(QRectF(30.5, 27, 13, 46), 2.6, 2.6);
        painter.drawRoundedRect(QRectF(56.5, 27, 13, 46), 2.6, 2.6);
        break;
    case Glyph::Resume:
        // Off centre to the right: a triangle's weight sits by its base.
        painter.drawPolygon(QPolygonF({ QPointF(35, 25), QPointF(35, 75), QPointF(75, 50) }));
        break;
    case Glyph::Stop: painter.drawRoundedRect(QRectF(30, 30, 40, 40), 6, 6); break;
    }
}

} // namespace

Colors colors(Glyph glyph, bool dark)
{
    const QColor darkGlyph(0x1B, 0x1B, 0x1B);
    if (glyph == Glyph::Pause)
        return dark ? Colors{ QColor(0xFF, 0xCA, 0x28), QColor(0xFF, 0xCA, 0x28), darkGlyph }
                    : Colors{ QColor(0xFB, 0xC0, 0x2D), QColor(0x8D, 0x6A, 0x00), darkGlyph };
    if (glyph == Glyph::Stop)
        return dark ? Colors{ QColor(0xFF, 0x6B, 0x6B), QColor(0xFF, 0x6B, 0x6B), darkGlyph }
                    : Colors{ QColor(0xC6, 0x28, 0x28), QColor(0xC6, 0x28, 0x28), Qt::white };
    // NewTab and Resume.
    return dark ? Colors{ QColor(0x66, 0xBB, 0x6A), QColor(0x66, 0xBB, 0x6A), darkGlyph }
                : Colors{ QColor(0x2E, 0x7D, 0x32), QColor(0x2E, 0x7D, 0x32), Qt::white };
}

QIcon icon(Glyph glyph, bool dark)
{
    const Colors palette = colors(glyph, dark);
    QPixmap pixmap(kPixmapSize, kPixmapSize);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(kPixmapSize / kGrid, kPixmapSize / kGrid);
    painter.setPen(Qt::NoPen);

    const QRectF disc(1.5, 1.5, kGrid - 3, kGrid - 3);
    painter.setBrush(palette.rim);
    painter.drawEllipse(disc);
    if (palette.rim != palette.disc) {
        painter.setBrush(palette.disc);
        painter.drawEllipse(disc.adjusted(kRimWidth, kRimWidth, -kRimWidth, -kRimWidth));
    }
    painter.setBrush(palette.glyph);
    drawGlyph(painter, glyph);
    painter.end();
    return QIcon(pixmap);
}

} // namespace PostingControlIcon
