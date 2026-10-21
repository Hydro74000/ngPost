//========================================================================
//
// Copyright (C) 2020 Matthieu Bruel <Matthieu.Bruel@gmail.com>
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

#include "SignedListWidget.h"
#include <QLabel>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QScrollBar>

SignedListWidget::SignedListWidget(QWidget *parent) :
    QListWidget(parent),
    _asciiLbl(new QLabel(this)),
    _fileIcon(":/icons/file.png"),
    _folderIcon(":/icons/folder.png")
{
    //    installEventFilter(this);
    // Only user moves on the scroll bar or the wheel trigger an action: scrollTo()
    // does not. Keys are caught in keyPressEvent().
    connect(verticalScrollBar(), &QAbstractSlider::actionTriggered, this, [this] {
        _scrolledAway = true;
    });
}

void SignedListWidget::followItem(QListWidgetItem *item)
{
    const QPersistentModelIndex previous = _followed;
    _followed = item ? QPersistentModelIndex(indexFromItem(item)) : QPersistentModelIndex();
    // Nothing to compute for a background tab: showEvent catches up.
    if (!_followed.isValid() || !isVisible())
        return;
    // The user went to look elsewhere: follow again once the row is back in sight.
    if (_scrolledAway && previous.isValid() && !_isShown(previous))
        return;
    _scrolledAway = false;
    scrollTo(_followed, PositionAtCenter);
}

int SignedListWidget::followedRow() const
{
    return _followed.isValid() ? _followed.row() : -1;
}

bool SignedListWidget::_isShown(const QModelIndex &index) const
{
    return viewport()->rect().intersects(visualRect(index));
}


void SignedListWidget::setSignature(const QString &str)
{
    _asciiLbl->setText(str);
    _sizeAscii = _asciiLbl->sizeHint();
}

void SignedListWidget::addPath(const QString &path, bool isDir)
{
    QListWidgetItem *item = new QListWidgetItem(isDir ? _folderIcon : _fileIcon, path);
    addItem(item);
    if (_asciiLbl->isVisible())
        _asciiLbl->hide();
}

bool SignedListWidget::addPathIfNotInList(const QString &path, int lastIndexToCheck, bool isDir)
{
    for (int i = 0 ; i < lastIndexToCheck ; ++i)
    {
        if (item(i)->text() == path)
            return false;
    }

    addPath(path, isDir);
    return true;
}

//#include <QDebug>
void SignedListWidget::removeItemWidget2(QListWidgetItem *item)
{
//    qDebug() << "[removeItemWidget2] count: " << count();
    takeItem(row(item));
    if (count() == 0)
    {
        _asciiLbl->show();
        emit empty();
    }
}

void SignedListWidget::clear2()
{
    clear();
    _asciiLbl->show();
}

void SignedListWidget::onDeleteSelectedItems()
{
    for (QListWidgetItem *item : selectedItems())
    {
        removeItemWidget2(item);
        delete item;
    }
}

void SignedListWidget::resizeEvent(QResizeEvent *e)
{
    QListView::resizeEvent(e);
    QSize listSize = e->size();
    _asciiLbl->setGeometry(listSize.width() - _sizeAscii.width(),
                           listSize.height() - _sizeAscii.height(),
                           _sizeAscii.width(),
                           _sizeAscii.height());
    if (_followed.isValid() && isVisible() && !_scrolledAway)
        scrollTo(_followed, PositionAtCenter);
}

void SignedListWidget::showEvent(QShowEvent *e)
{
    QListWidget::showEvent(e);
    // Back on this tab: the post went on meanwhile, the old position is stale.
    _scrolledAway = false;
    if (_followed.isValid())
        scrollTo(_followed, PositionAtCenter);
}

void SignedListWidget::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace)
    {
        onDeleteSelectedItems();
        e->accept();
        return;
    }
    const int position = verticalScrollBar()->value();
    QListWidget::keyPressEvent(e);
    // Home, Page Down...: no scroll bar action, but the user went to look too.
    if (verticalScrollBar()->value() != position)
        _scrolledAway = true;
}

void SignedListWidget::mousePressEvent(QMouseEvent *e)
{
    QListWidget::mousePressEvent(e);
    if (e->button() == Qt::RightButton)
    {
        emit rightClick();
        e->accept();
    }
}

bool SignedListWidget::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::KeyPress)
    {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        if(keyEvent->key() == Qt::Key_Delete || keyEvent->key() == Qt::Key_Backspace)
            onDeleteSelectedItems();
    }
    return QListWidget::eventFilter(obj, event);       
}
