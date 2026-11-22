// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// Small icons of the queue manager, drawn with QPainter so they follow the
// palette and need neither an emoji font nor new image resources.
//
//========================================================================

#ifndef HMI_QUEUEICONS_H
#define HMI_QUEUEICONS_H

#include <QColor>
#include <QIcon>

namespace queueicons
{

enum class Glyph {
    Play,
    Pause,
    Stop,
    Next,
    Gear,
    Box,
    Clock,
    Warn,
    Lock,
    Up,
    Down,
    Top,
    Bottom,
    Trash,
    Edit,
    Search,
    Expand,
    PopOut,
    Chevron,
    Plus,
    List,
    Resume,
    Folder
};

QIcon icon(Glyph glyph, const QColor &color, int size = 16);

} // namespace queueicons

#endif // HMI_QUEUEICONS_H
