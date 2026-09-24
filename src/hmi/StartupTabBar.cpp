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

#include "StartupTabBar.h"

#include <QEvent>
#include <QFontMetrics>
#include <QKeySequence>
#include <QMouseEvent>
#include <QStyleOption>
#include <QStylePainter>
#include <QToolButton>

StartupTabBar::StartupTabBar(QWidget *parent)
    : QTabBar(parent)
{
    // QTabBar makes its arrows in its constructor and never replaces them.
    _scrollLeft = findChild<QToolButton *>(QStringLiteral("ScrollLeftButton"),
                                           Qt::FindDirectChildrenOnly);
    _scrollRight = findChild<QToolButton *>(QStringLiteral("ScrollRightButton"),
                                            Qt::FindDirectChildrenOnly);
    for (QToolButton *arrow : { _scrollLeft, _scrollRight })
        if (arrow)
            arrow->installEventFilter(this);
    _retranslate();
}

void StartupTabBar::setStartupTab(int index)
{
    if (_startupTab == index)
        return;
    _startupTab = index;
    // tabSizeHint() already reserves the bold width on every tab, so nothing
    // moves and nothing has to be laid out again: a repaint is the whole
    // refresh.
    update();
}

void StartupTabBar::scrollToEnd(bool toEnd)
{
    // Each click scrolls one tab further, and Qt disables the arrow once the
    // bar has reached that end.
    QToolButton *arrow = toEnd ? _scrollRight : _scrollLeft;
    for (int i = 0; arrow && arrow->isEnabled() && i < count(); ++i)
        arrow->click();
}

void StartupTabBar::changeEvent(QEvent *event)
{
    QTabBar::changeEvent(event);
    if (event->type() == QEvent::LanguageChange)
        _retranslate();
}

bool StartupTabBar::eventFilter(QObject *watched, QEvent *event)
{
    const bool arrow = watched && (watched == _scrollLeft || watched == _scrollRight);
    if (arrow && event->type() == QEvent::MouseButtonPress) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton && mouse->modifiers() & Qt::ControlModifier) {
            scrollToEnd(watched == _scrollRight);
            return true; // the arrow never goes down, so no plain click follows
        }
    }
    return QTabBar::eventFilter(watched, event);
}

void StartupTabBar::_retranslate()
{
    // "Ctrl+" here, "⌘" on macOS, where Qt::ControlModifier is Command.
    const QString ctrl = QKeySequence(Qt::CTRL).toString(QKeySequence::NativeText);
    if (_scrollLeft)
        _scrollLeft->setToolTip(tr("Scroll left\n%1click: back to the first tab").arg(ctrl));
    if (_scrollRight)
        _scrollRight->setToolTip(tr("Scroll right\n%1click: on to the last tab").arg(ctrl));
}

void StartupTabBar::paintEvent(QPaintEvent *)
{
    // No base frame to draw here: the bar is owned by a StartupTabWidget,
    // which turns drawBase() off and draws that edge itself as the pane below.
    // Tabs are not movable either, so there is no tab being dragged to paint
    // apart from the others -- which is what is left of QTabBar::paintEvent.
    QStylePainter painter(this);
    const int     current = currentIndex();

    QFont normalFont = font(), boldFont = font();
    boldFont.setBold(true);

    auto paintTab = [&](int index) {
        if (!isTabVisible(index))
            return;
        QStyleOptionTab tab;
        initStyleOption(&tab, index);
        painter.setFont(index == _startupTab ? boldFont : normalFont);
        painter.drawControl(QStyle::CE_TabBarTab, tab);
    };

    // The selected tab overlaps its neighbours (the style sheet gives it
    // negative margins), so it is painted last -- like QTabBar does.
    for (int i = 0; i < count(); ++i)
        if (i != current)
            paintTab(i);
    if (current >= 0)
        paintTab(current);
}

QSize StartupTabBar::tabSizeHint(int index) const
{
    QSize size = QTabBar::tabSizeHint(index);

    // Every tab is measured as if its title were bold. Pinning or unpinning a
    // tab then leaves the whole bar where it was, and a bold title is never
    // clipped by a width computed for a regular one.
    QFont bold = font();
    bold.setBold(true);
    QString const text = tabText(index);
    size.setWidth(size.width() + QFontMetrics(bold).horizontalAdvance(text)
                  - QFontMetrics(font()).horizontalAdvance(text));
    return size;
}

QSize StartupTabBar::minimumTabSizeHint(int index) const
{
    QSize size = tabSizeHint(index);
    QString const text = tabText(index);
    qsizetype const number = text.lastIndexOf(QLatin1Char('#'));
    if (number <= 0)
        return size;

    // What is left once elided from the left (see the ElideLeft MainWindow
    // gives the bar), measured bold like tabSizeHint() does.
    QFont bold = font();
    bold.setBold(true);
    QFontMetrics const metrics(bold);
    QString const shortest = QChar(0x2026) + text.mid(number);
    size.setWidth(size.width() - metrics.horizontalAdvance(text)
                  + metrics.horizontalAdvance(shortest));
    return size;
}

StartupTabWidget::StartupTabWidget(QWidget *parent) : QTabWidget(parent)
{
    // Same setup QTabWidget gives its own bar: the object name is what the
    // style sheet machinery looks for, and the pane draws the base.
    StartupTabBar *bar = new StartupTabBar(this);
    bar->setObjectName(QStringLiteral("qt_tabwidget_tabbar"));
    bar->setDrawBase(false);
    setTabBar(bar);
}

StartupTabBar *StartupTabWidget::startupTabBar() const
{
    return static_cast<StartupTabBar *>(tabBar());
}
