// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>

#include "OwnerLock.h"

#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QRegularExpression>
#include <QUuid>

namespace queue
{

namespace
{

//! Same database, same locks: a symbolic link must not give two lock spaces.
QString canonicalBase(const QString &dbPath)
{
    const QFileInfo info(dbPath);
    if (info.exists())
        return info.canonicalFilePath();
    const QFileInfo dir(info.absolutePath());
    const QString parent = dir.exists() ? dir.canonicalFilePath() : dir.absoluteFilePath();
    return QDir(parent).filePath(info.fileName());
}

} // namespace

OwnerLock::OwnerLock(const QString &dbPath)
    : _dbPath(dbPath)
{
}

OwnerLock::~OwnerLock() = default;

bool OwnerLock::isValidOwnerId(const QString &ownerId)
{
    static const QRegularExpression shape(
        QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
    return shape.match(ownerId).hasMatch();
}

QString OwnerLock::lockPath(const QString &dbPath, const QString &ownerId)
{
    return QStringLiteral("%1.owner-%2.lock").arg(canonicalBase(dbPath), ownerId);
}

bool OwnerLock::acquire(QString *error)
{
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto lock = std::make_unique<QLockFile>(lockPath(_dbPath, id));
    lock->setStaleLockTime(0); // never stale by age: only a dead process frees it
    if (!lock->tryLock(0)) {
        if (error)
            *error = QStringLiteral("cannot create the owner lock %1").arg(lockPath(_dbPath, id));
        return false;
    }
    _ownerId = id;
    _lock = std::move(lock);
    return true;
}

bool OwnerLock::isHeld() const
{
    return _lock && _lock->isLocked();
}

QStringList OwnerLock::ownersOnDisk(const QString &dbPath)
{
    const QFileInfo base(canonicalBase(dbPath));
    const QString prefix = base.fileName() + QStringLiteral(".owner-");
    QStringList owners;
    const QStringList files = QDir(base.absolutePath())
                                  .entryList({ prefix + QStringLiteral("*.lock") },
                                             QDir::Files | QDir::Hidden);
    for (const QString &file : files) {
        const QString id = file.mid(prefix.size(), file.size() - prefix.size() - 5);
        if (isValidOwnerId(id))
            owners << id;
    }
    return owners;
}

OwnerLock::Vitality OwnerLock::probe(const QString &dbPath,
                                     const QString &ownerId,
                                     std::unique_ptr<QLockFile> *guard)
{
    if (!isValidOwnerId(ownerId))
        return Vitality::Unknown;
    auto lock = std::make_unique<QLockFile>(lockPath(dbPath, ownerId));
    lock->setStaleLockTime(0);
    if (lock->tryLock(0)) {
        if (guard)
            *guard = std::move(lock);
        return Vitality::Dead;
    }
    return lock->error() == QLockFile::LockFailedError ? Vitality::Alive : Vitality::Unknown;
}

} // namespace queue
