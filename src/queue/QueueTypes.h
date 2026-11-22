// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// Vocabulary of the queue manager (plan_queue.md §3).
//
// The queue holds entries as DATA. Only the entry being transferred and the
// one being prepared ever own a runtime (a PostingJob); every other entry is
// visible, diagnosable and removable without one. Nothing here depends on Qt
// Widgets: the same model runs in the headless build.
//
//========================================================================

#ifndef QUEUE_QUEUETYPES_H
#define QUEUE_QUEUETYPES_H

#include <QHash>
#include <QHashFunctions>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QVariantMap>

namespace queue
{

//! Where an entry came from. Explicit: "no widget" no longer means "Auto".
enum class Origin {
    Quick,
    Auto,
    Cli,
    Resume,
    Drop
};

//! Life cycle of an entry. Held and blocked are orthogonal flags, never states.
enum class Lifecycle {
    Queued,     //!< waiting; may carry an engaged transfer manifest (a resume)
    Preparing,  //!< rar / par2 / collection running in the preparation slot
    Prepared,   //!< verified artifacts wait for the transfer slot
    Posting,    //!< owns the transfer slot
    Quiescing,  //!< being stopped: tools, connections, NNTP barrier, flush
    Finalizing, //!< F1/F2/F3 in progress; cannot be preempted
    Recovering  //!< start-up recovery of sources and artifacts
};

//! Which part of the persisted queues an entry belongs to.
enum class Domain {
    Session,  //!< the queue this process runs
    Deferred, //!< a restored lot set aside ("Non restaurés"); never executed
    Foreign   //!< another live instance's queue; read only
};

//! The four classes of options of plan_queue.md §3.7. They decide what an edit
//! invalidates and what stays editable once a transfer manifest is engaged.
enum class FieldClass {
    ArtifactRecipe,
    TransferGeometry,
    Outputs,
    Operation
};

//! A preparation is a chain of steps the scheduler drives one at a time, so a
//! global pause can fall between rar and par2.
enum class PrepStep {
    Compress,
    Par2,
    Collect
};

//! What an interruption means for the entry being stopped.
enum class Disposition {
    Keep,   //!< interruption kept: back to Queued, a new run follows
    Stop,   //!< terminal stop: F1/F2/F3, stays resumable from History
    Abandon //!< planning abandoned: facts kept, no deliverable
};

enum class Edge {
    Top,
    Bottom
};

//! How a removal treats prepared archives. keepRar is honoured in every case.
enum class RemovePolicy {
    Default,
    DiscardPreparedArchives
};

//! A technical reason that keeps an entry from running. A stable code is
//! translated by the view; parameters never contain a secret.
struct BlockReason
{
    QString code;
    QVariantMap params;

    bool isBlocked() const { return !code.isEmpty(); }
    bool operator==(const BlockReason &other) const
    {
        return code == other.code && params == other.params;
    }
};

//! One set of prepared archives. A set belongs to a post, has a generation
//! number of its own and is only usable while its recipe is still current.
struct ArtifactSetRef
{
    QString id;
    int generation = 0;
    quint64 recipeRevision = 0;
    qint64 bytes = 0;
    QString root; //!< effective TMP_DIR or TMP_RAM recorded at creation

    bool isNull() const { return id.isEmpty(); }
};

//! "Post now": a durable intent, at most one per queue (plan_queue.md §3.5).
struct PostNowIntent
{
    enum class State {
        Pending,
        Switching
    };

    QString intentId;
    QString queueId;
    qint64 targetPostId = 0;
    quint64 targetRecipeRevision = 0;
    QString activeRunId;
    State state = State::Pending;

    bool isNull() const { return intentId.isEmpty(); }
};

//! What a view displayed when it issued a command. A command whose view is
//! stale is rejected rather than applied to an order the user never saw.
struct QueueView
{
    QString queueId;
    Domain domain = Domain::Session;
    quint64 orderRevision = 0;
    QHash<qint64, quint64> recipeRevisions; //!< post id -> recipe revision seen
};

struct QueueCommandResult
{
    enum class Status {
        Done,
        InProgress,
        Rejected
    };

    Status status = Status::Rejected;
    QList<qint64> postIds;
    QString errorCode; //!< stable code, e.g. "stale_view", "store_failed"
    QString message;

    bool ok() const { return status != Status::Rejected; }

    static QueueCommandResult done(QList<qint64> ids = {})
    {
        return { Status::Done, std::move(ids), {}, {} };
    }
    static QueueCommandResult inProgress(QList<qint64> ids)
    {
        return { Status::InProgress, std::move(ids), {}, {} };
    }
    static QueueCommandResult rejected(const QString &code, const QString &msg = {})
    {
        return { Status::Rejected, {}, code, msg };
    }
};

inline size_t qHash(FieldClass value, size_t seed = 0) noexcept
{
    return ::qHash(static_cast<int>(value), seed);
}

QString originName(Origin origin);
bool parseOrigin(const QString &text, Origin &origin);
QString lifecycleName(Lifecycle lifecycle);
bool parseLifecycle(const QString &text, Lifecycle &lifecycle);

} // namespace queue

Q_DECLARE_METATYPE(queue::Domain)
Q_DECLARE_METATYPE(queue::PrepStep)

#endif // QUEUE_QUEUETYPES_H
