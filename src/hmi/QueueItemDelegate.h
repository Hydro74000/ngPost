// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// Paints the rows of the queue manager (plan_queue.md §4.3): a grip and the
// rank, state chips with icon + text + colour (never colour alone), a thin
// progress bar, the reason of a block. Colours derive from the palette so
// dark and light themes both stay readable.
//
//========================================================================

#ifndef HMI_QUEUEITEMDELEGATE_H
#define HMI_QUEUEITEMDELEGATE_H

#include <QStyledItemDelegate>

class QueueTableModel;

class QueueItemDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    explicit QueueItemDelegate(QueueTableModel *model, QObject *parent = nullptr);

    void paint(QPainter *painter,
               const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    QueueTableModel *_model;
};

#endif // HMI_QUEUEITEMDELEGATE_H
