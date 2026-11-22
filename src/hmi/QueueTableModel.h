// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// Table model of the queue manager (plan_queue.md §4.3). It shows one
// domain of a PostQueue and remembers the revisions it displayed, so a
// command issued from the view is checked against what the user saw.
// Live progress comes from the QueueExecutor and is never used to decide.
//
//========================================================================

#ifndef HMI_QUEUETABLEMODEL_H
#define HMI_QUEUETABLEMODEL_H

#include "queue/PostQueue.h"
#include "queue/QueueExecutor.h"

#include <QAbstractTableModel>
#include <QHash>
#include <QList>

class QueueTableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column {
        RankColumn,
        PostColumn,
        OriginColumn,
        StateColumn,
        ProgressColumn,
        SizeColumn,
        AddedColumn,
        ColumnCount
    };
    enum Role {
        PostIdRole = Qt::UserRole + 1,
        PercentRole
    };

    //! Live figures of an entry, for display only.
    struct Live
    {
        int percent = -1;
        qint64 bytesPerSecond = 0;
        queue::PrepStep step = queue::PrepStep::Compress;
    };

    QueueTableModel(queue::PostQueue *queue,
                    queue::QueueExecutor *executor,
                    QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    Qt::DropActions supportedDropActions() const override;
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    bool canDropMimeData(const QMimeData *data,
                         Qt::DropAction action,
                         int row,
                         int column,
                         const QModelIndex &parent) const override;
    bool dropMimeData(const QMimeData *data,
                      Qt::DropAction action,
                      int row,
                      int column,
                      const QModelIndex &parent) override;

    const queue::QueueView &shownView() const { return _view; }
    const queue::QueueEntry *entryAt(int row) const;
    qint64 postIdAt(int row) const;
    int rowOf(qint64 postId) const;
    Live live(qint64 postId) const { return _live.value(postId); }
    queue::PostQueue *queue() const { return _queue; }
    //! Drag and drop reorders only when nothing is filtered out.
    void setReorderEnabled(bool enabled) { _reorderEnabled = enabled; }

    // Texts shared with the delegate and the panel (translated).
    static QString stateText(const queue::QueueEntry &entry);
    static QString originText(const queue::QueueEntry &entry);
    static QString blockText(const queue::BlockReason &reason);
    static QString sizeText(qint64 bytes);
    QString detailText(const queue::QueueEntry &entry) const;

public slots:
    void refresh();

signals:
    void moveRequested(const QList<qint64> &ids, int targetRow);
    void filesDropped(const QStringList &paths);
    //! Emitted around a reset, so the panel can keep the selection by post id.
    void aboutToReset();
    void wasReset();

private:
    void scheduleRefresh();
    void onProgress(qint64 postId, int percent, qint64 bytesPerSecond);
    void onStepProgress(qint64 postId, queue::PrepStep step, int percent);
    void updateRow(qint64 postId);

    queue::PostQueue *_queue;
    QList<queue::QueueEntry> _rows;
    queue::QueueView _view;
    QHash<qint64, Live> _live;
    bool _refreshScheduled = false;
    bool _reorderEnabled = true;
};

#endif // HMI_QUEUETABLEMODEL_H
