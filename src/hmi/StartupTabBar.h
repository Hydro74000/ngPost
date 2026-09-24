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

#ifndef STARTUPTABBAR_H
#define STARTUPTABBAR_H

#include <QTabBar>
#include <QTabWidget>

class QToolButton;

//! Tab bar that writes one title in bold: the tab the user picked to be
//! opened at startup (tab context menu, "Open this tab on startup").
//!
//! QTabBar has no per tab font -- there is a setTabTextColor(), there is no
//! setTabFont() next to it -- and a QProxyStyle is no help either: the tab
//! widget carries a style sheet, and QStyleSheetStyle paints a tab whose rule
//! has a border of its own without ever asking the base style. What is left
//! is to run the paint loop here: the style still draws each tab, we only
//! choose the font it draws each one with.
//!
//! A Ctrl+click on one of the scroll arrows runs the bar to that end.
class StartupTabBar : public QTabBar
{
    Q_OBJECT

public:
    explicit StartupTabBar(QWidget *parent = nullptr);

    //! Index of the tab written in bold, -1 when no tab is pinned.
    int  startupTab() const { return _startupTab; }

    //! Move the bold to \a index (-1 to remove it) and repaint.
    void setStartupTab(int index);

    //! Scroll the bar to its first tab, or to its last one when \a toEnd;
    //! the current tab stays the same.
    void scrollToEnd(bool toEnd);

protected:
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void  paintEvent(QPaintEvent *event) override;
    QSize tabSizeHint(int index) const override;
    //! How far a full bar may shrink a tab before scrolling: down to "…#N" for
    //! a title ending with a number, not at all for the others. Qt's own floor
    //! is three letters and an ellipsis, which leaves every quick post tab
    //! looking the same.
    QSize minimumTabSizeHint(int index) const override;

private:
    void _retranslate();

    int _startupTab = -1;
    //! QTabBar's own scroll arrows, found by the names Qt gives them.
    QToolButton *_scrollLeft = nullptr;
    QToolButton *_scrollRight = nullptr;
};

//! The tab widget holding a StartupTabBar. QTabWidget::setTabBar() is
//! protected and wants to be called before the first tab is added, so the bar
//! is installed here in the constructor: MainWindow.ui promotes its
//! postTabWidget to this class.
class StartupTabWidget : public QTabWidget
{
    Q_OBJECT

public:
    explicit StartupTabWidget(QWidget *parent = nullptr);

    StartupTabBar *startupTabBar() const;
};

#endif // STARTUPTABBAR_H
