// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// Who owns what in a shared history database (plan_queue.md invariant 7,
// §5.2). Every process holds a lock file <POST_DB>.owner-<id>.lock for as
// long as it may write; another process is proven gone only when its lock
// can actually be taken. A missing answer is "unknown", never "dead".
//
//========================================================================

#ifndef QUEUE_OWNERLOCK_H
#define QUEUE_OWNERLOCK_H

#include <QString>
#include <QStringList>

#include <memory>

class QLockFile;

namespace queue
{

class OwnerLock
{
public:
    enum class Vitality {
        Alive,
        Dead,
        Unknown
    };

    explicit OwnerLock(const QString &dbPath);
    ~OwnerLock();

    //! Draws a new owner id and takes its lock; false leaves no owner.
    bool acquire(QString *error = nullptr);
    QString ownerId() const { return _ownerId; }
    bool isHeld() const;

    //! Lock files of every owner registered next to the database.
    static QStringList ownersOnDisk(const QString &dbPath);
    //! Tries to take another owner's lock. Dead: @p guard keeps it held, so the
    //! owner cannot come back until the caller has committed its claim.
    static Vitality probe(const QString &dbPath,
                          const QString &ownerId,
                          std::unique_ptr<QLockFile> *guard = nullptr);
    //! An owner id read from the database is used in a file name only when it
    //! has the exact shape this class generates.
    static bool isValidOwnerId(const QString &ownerId);
    static QString lockPath(const QString &dbPath, const QString &ownerId);

private:
    QString _dbPath;
    QString _ownerId;
    std::unique_ptr<QLockFile> _lock;
};

} // namespace queue

#endif // QUEUE_OWNERLOCK_H
