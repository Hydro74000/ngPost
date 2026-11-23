// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>

#include "QueueTableModel.h"

#include <QDateTime>
#include <QLocale>
#include <QMimeData>
#include <QSet>
#include <QTimer>
#include <QUrl>

using namespace queue;

namespace
{

const QString kIdsMime = QStringLiteral("application/x-ngpost-queue-ids");

QString stepName(PrepStep step)
{
    switch (step) {
    case PrepStep::Compress: return QStringLiteral("RAR");
    case PrepStep::Par2: return QStringLiteral("PAR2");
    case PrepStep::Collect: return QueueTableModel::tr("Checking");
    }
    return {};
}

} // namespace

QueueTableModel::QueueTableModel(PostQueue *queue, QueueExecutor *executor, QObject *parent)
    : QAbstractTableModel(parent)
    , _queue(queue)
{
    connect(_queue, &PostQueue::changed, this, &QueueTableModel::scheduleRefresh);
    connect(_queue, &PostQueue::intentChanged, this, &QueueTableModel::scheduleRefresh);
    if (executor) {
        connect(executor, &QueueExecutor::progress, this, &QueueTableModel::onProgress);
        connect(executor, &QueueExecutor::stepProgress, this, &QueueTableModel::onStepProgress);
    }
    _rows = _queue->snapshot(Domain::Session);
    _view = _queue->view(Domain::Session);
}

void QueueTableModel::scheduleRefresh()
{
    if (_refreshScheduled)
        return;
    _refreshScheduled = true;
    QTimer::singleShot(0, this, &QueueTableModel::refresh);
}

void QueueTableModel::refresh()
{
    _refreshScheduled = false;
    QList<QueueEntry> next = _queue->snapshot(Domain::Session);
    QList<qint64> oldIds, newIds;
    for (const QueueEntry &e : _rows)
        oldIds << e.postId;
    for (const QueueEntry &e : next)
        newIds << e.postId;
    _view = _queue->view(Domain::Session);
    for (auto it = _live.begin(); it != _live.end();)
        it = newIds.contains(it.key()) ? std::next(it) : _live.erase(it);

    if (oldIds == newIds) {
        _rows = next;
        if (!_rows.isEmpty())
            emit dataChanged(index(0, 0),
                             index(static_cast<int>(_rows.size()) - 1, ColumnCount - 1));
        return;
    }
    if (QSet<qint64>(oldIds.cbegin(), oldIds.cend())
        == QSet<qint64>(newIds.cbegin(), newIds.cend())) {
        // Same entries, new order: keep selection and current index by post id.
        emit layoutAboutToBeChanged();
        const QModelIndexList from = persistentIndexList();
        _rows = next;
        QModelIndexList to;
        for (const QModelIndex &idx : from)
            to << index(static_cast<int>(newIds.indexOf(oldIds.value(idx.row()))), idx.column());
        changePersistentIndexList(from, to);
        emit layoutChanged();
        return;
    }
    emit aboutToReset();
    beginResetModel();
    _rows = next;
    endResetModel();
    emit wasReset();
}

void QueueTableModel::onProgress(qint64 postId, int percent, qint64 bytesPerSecond)
{
    Live &l = _live[postId];
    l.percent = percent;
    l.bytesPerSecond = bytesPerSecond;
    updateRow(postId);
}

void QueueTableModel::onStepProgress(qint64 postId, PrepStep step, int percent)
{
    Live &l = _live[postId];
    l.percent = percent;
    l.step = step;
    updateRow(postId);
}

void QueueTableModel::updateRow(qint64 postId)
{
    const int row = rowOf(postId);
    if (row >= 0)
        emit dataChanged(index(row, StateColumn), index(row, ProgressColumn));
}

// --- access --------------------------------------------------------------------------

const QueueEntry *QueueTableModel::entryAt(int row) const
{
    return row >= 0 && row < _rows.size() ? &_rows.at(row) : nullptr;
}

qint64 QueueTableModel::postIdAt(int row) const
{
    const QueueEntry *e = entryAt(row);
    return e ? e->postId : 0;
}

int QueueTableModel::rowOf(qint64 postId) const
{
    for (int i = 0; i < _rows.size(); ++i)
        if (_rows.at(i).postId == postId)
            return i;
    return -1;
}

int QueueTableModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(_rows.size());
}

int QueueTableModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

// --- texts ---------------------------------------------------------------------------

QString QueueTableModel::sizeText(qint64 bytes)
{
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

QString QueueTableModel::originText(const QueueEntry &entry)
{
    switch (entry.origin) {
    case Origin::Quick:
        return entry.tabNumber > 0 ? tr("Tab #%1").arg(entry.tabNumber) : tr("Quick");
    case Origin::Auto: return tr("Auto");
    case Origin::Cli: return tr("CLI");
    case Origin::Resume: return tr("Resume");
    case Origin::Drop: return tr("Drop");
    }
    return {};
}

QString QueueTableModel::stateText(const QueueEntry &entry)
{
    if (entry.blocked.isBlocked())
        return tr("Blocked");
    switch (entry.lifecycle) {
    case Lifecycle::Posting: return tr("Posting");
    case Lifecycle::Preparing: return tr("Preparing");
    case Lifecycle::Prepared: return entry.held ? tr("Held · ready") : tr("Ready");
    case Lifecycle::Quiescing: return tr("Stopping");
    case Lifecycle::Finalizing: return tr("Finalizing");
    case Lifecycle::Recovering: return tr("Recovering");
    case Lifecycle::Queued: return entry.held ? tr("Held") : tr("Queued");
    }
    return {};
}

QString QueueTableModel::blockText(const BlockReason &reason)
{
    static const QHash<QString, const char *> texts{
        { QStringLiteral("resources"), QT_TR_NOOP("Not enough temporary space") },
        { QStringLiteral("preparation_failed"), QT_TR_NOOP("Archive or PAR2 creation failed") },
        { QStringLiteral("vpn_unavailable"), QT_TR_NOOP("The VPN is unavailable") },
        { QStringLiteral("finalization_failed"), QT_TR_NOOP("The NZB could not be written") },
        { QStringLiteral("quiesce_failed"), QT_TR_NOOP("The stop could not be confirmed") },
        { QStringLiteral("tool_unproven"),
          QT_TR_NOOP("Its archive or PAR2 tool could not be confirmed stopped") },
        { QStringLiteral("password_required"), QT_TR_NOOP("Password to enter again") },
        { QStringLiteral("sources_pending_recovery"),
          QT_TR_NOOP("Waiting for its sources to be restored") },
        { QStringLiteral("sources_missing"), QT_TR_NOOP("A source file is missing or changed") },
    };
    const char *text = texts.value(reason.code, nullptr);
    return text ? tr(text) : reason.code;
}

QString QueueTableModel::detailText(const QueueEntry &entry) const
{
    const Live l = _live.value(entry.postId);
    const bool postNowFor = _queue->intent().targetPostId == entry.postId;
    if (entry.blocked.isBlocked())
        return blockText(entry.blocked);
    switch (entry.lifecycle) {
    case Lifecycle::Posting:
        return l.percent < 0 ? tr("Starting…")
                             : tr("%1 % · %2/s").arg(l.percent).arg(sizeText(l.bytesPerSecond));
    case Lifecycle::Preparing: {
        const QString step = tr("%1 %2 %").arg(stepName(l.step)).arg(std::max(0, l.percent));
        return postNowFor ? tr("%1 · posts as soon as ready").arg(step) : step;
    }
    case Lifecycle::Prepared:
        return tr("Archives ready · %1 in temp").arg(sizeText(entry.artifacts.bytes));
    case Lifecycle::Quiescing: return tr("Stopping…");
    case Lifecycle::Finalizing: return tr("Writing the NZB…");
    case Lifecycle::Recovering: return tr("Recovering sources and archives…");
    case Lifecycle::Queued: break;
    }
    if (entry.held)
        return entry.transferManifestEngaged ? tr("Resume · held by you") : tr("Held by you");
    if (postNowFor)
        return tr("Post now: waiting for its preparation");
    if (entry.transferManifestEngaged)
        return tr("Resume: confirmed articles are not posted again");
    return {};
}

// --- QAbstractItemModel ---------------------------------------------------------------------

QVariant QueueTableModel::data(const QModelIndex &index, int role) const
{
    const QueueEntry *e = entryAt(index.row());
    if (!e || !index.isValid())
        return {};
    if (role == PostIdRole)
        return e->postId;
    if (role == PercentRole)
        return _live.value(e->postId).percent;
    if (role == Qt::TextAlignmentRole && index.column() == SizeColumn)
        return QVariant(Qt::AlignRight | Qt::AlignVCenter);
    if (role == Qt::ToolTipRole && index.column() == PostColumn)
        return e->options.job.nzbFilePath;
    if (role == Qt::AccessibleTextRole)
        return tr("Position %1: %2, %3, %4, %5")
            .arg(index.row() + 1)
            .arg(e->name, originText(*e), stateText(*e), sizeText(e->sourceBytes));
    if (role != Qt::DisplayRole)
        return {};
    switch (index.column()) {
    case RankColumn: return index.row() + 1;
    case PostColumn: return e->name;
    case OriginColumn: return originText(*e);
    case StateColumn: return stateText(*e);
    case ProgressColumn: return detailText(*e);
    case SizeColumn: return sizeText(e->sourceBytes);
    case AddedColumn: {
        const QDateTime local = e->enqueuedAt.toLocalTime();
        return local.date() == QDate::currentDate()
            ? QLocale().toString(local.time(), QLocale::ShortFormat)
            : QLocale().toString(local, QLocale::ShortFormat);
    }
    default: return {};
    }
}

QVariant QueueTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case RankColumn: return QStringLiteral("#");
    case PostColumn: return tr("Post");
    case OriginColumn: return tr("Origin");
    case StateColumn: return tr("State");
    case ProgressColumn: return tr("Progress / details");
    case SizeColumn: return tr("Size");
    case AddedColumn: return tr("Added");
    default: return {};
    }
}

Qt::ItemFlags QueueTableModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return _reorderEnabled ? Qt::ItemIsDropEnabled : Qt::NoItemFlags;
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    const QueueEntry *e = entryAt(index.row());
    if (_reorderEnabled && e && _queue->capabilityOf(Action::Move, e->postId).allowed)
        f |= Qt::ItemIsDragEnabled;
    return f;
}

// --- drag and drop ----------------------------------------------------------------------------

Qt::DropActions QueueTableModel::supportedDropActions() const
{
    return Qt::MoveAction | Qt::CopyAction;
}

QStringList QueueTableModel::mimeTypes() const
{
    return { kIdsMime, QStringLiteral("text/uri-list") };
}

QMimeData *QueueTableModel::mimeData(const QModelIndexList &indexes) const
{
    QSet<int> rows;
    for (const QModelIndex &idx : indexes)
        rows.insert(idx.row());
    QList<int> sorted(rows.cbegin(), rows.cend());
    std::sort(sorted.begin(), sorted.end());
    QStringList ids;
    for (int row : sorted)
        ids << QString::number(postIdAt(row));
    auto *mime = new QMimeData;
    mime->setData(kIdsMime, ids.join(QLatin1Char(',')).toLatin1());
    return mime;
}

bool QueueTableModel::canDropMimeData(const QMimeData *data,
                                      Qt::DropAction action,
                                      int row,
                                      int column,
                                      const QModelIndex &parent) const
{
    Q_UNUSED(row)
    Q_UNUSED(column)
    Q_UNUSED(parent)
    if (data->hasFormat(kIdsMime))
        return _reorderEnabled && action == Qt::MoveAction;
    return data->hasUrls();
}

bool QueueTableModel::dropMimeData(const QMimeData *data,
                                   Qt::DropAction action,
                                   int row,
                                   int column,
                                   const QModelIndex &parent)
{
    if (!canDropMimeData(data, action, row, column, parent))
        return false;
    if (data->hasFormat(kIdsMime)) {
        QList<qint64> ids;
        for (const QByteArray &part : data->data(kIdsMime).split(','))
            ids << part.toLongLong();
        const int target = row >= 0 ? row : parent.isValid() ? parent.row() : rowCount();
        emit moveRequested(ids, target);
        return false; // the queue reorders itself; the view must not remove rows
    }
    QStringList paths;
    for (const QUrl &url : data->urls())
        if (url.isLocalFile())
            paths << url.toLocalFile();
    if (!paths.isEmpty())
        emit filesDropped(paths);
    return !paths.isEmpty();
}
