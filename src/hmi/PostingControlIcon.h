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

#ifndef POSTINGCONTROLICON_H
#define POSTINGCONTROLICON_H

#include <QColor>
#include <QIcon>

//! The coloured discs of the controls beside the tab bar: a green "+" for a
//! new tab, a yellow Pause that turns into a green Resume, a red Stop all.
//!
//! Each colour comes in two shades. On a light palette the disc is dark enough
//! to carry a white glyph; yellow never is, so it keeps a dark glyph and gets
//! an ochre rim, the part of it that stands out from a light button. On a dark
//! palette the discs are lighter and the glyphs dark: a white glyph would be
//! too faint on a disc bright enough to stand out from a dark button.
//! Every pair keeps 4.5:1 between glyph and disc, and 3:1 between the disc
//! (or its rim) and the button (WCAG non-text contrast).
namespace PostingControlIcon
{

enum class Glyph {
    NewTab,
    Pause,
    Resume,
    Stop
};

struct Colors
{
    QColor disc;
    QColor rim; //!< the disc colour when there is no rim
    QColor glyph;
};

Colors colors(Glyph glyph, bool dark);
QIcon icon(Glyph glyph, bool dark);

} // namespace PostingControlIcon

#endif // POSTINGCONTROLICON_H
