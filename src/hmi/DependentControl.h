//========================================================================
//
// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// GNU General Public License v3.
//
//========================================================================

#ifndef DEPENDENTCONTROL_H
#define DEPENDENTCONTROL_H

#include <QString>
#include <QWidget>

//! Greys a control that only means something while another box is ticked, and
//! says so. A greyed control with no explanation reads as a broken one: that is
//! exactly how "Limit RAR Number" and "Keep Archives" were reported as no
//! longer modifiable. Qt does show the tooltip of a disabled widget, so the
//! requirement is legible at the moment the control refuses to move.
//!
//! The help text is passed in every call rather than remembered, so a language
//! change only has to call the toggle handler again to rebuild both halves.
inline void setDependentEnabled(QWidget *widget, bool enabled,
                                QString const &help, QString const &requirement)
{
    if (!widget)
        return;

    widget->setEnabled(enabled);
    if (enabled)
        widget->setToolTip(help);
    else
        widget->setToolTip(help.isEmpty() ? requirement
                                          : help + QStringLiteral("\n\n") + requirement);
}

#endif // DEPENDENTCONTROL_H
