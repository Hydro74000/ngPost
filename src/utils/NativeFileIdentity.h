// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// The identity a file keeps through a rename on its file system: (device,
// inode) on POSIX, (volume serial, file index) on Windows, plus its size and
// modification time (plan_queue.md §6.3 "Identité de fichier du journal").
// A recovery trusts a path only when it still names the file it recorded.
//
//========================================================================

#ifndef UTILS_NATIVEFILEIDENTITY_H
#define UTILS_NATIVEFILEIDENTITY_H

#include <QString>
#include <QtGlobal>

#include <optional>

struct NativeFileIdentity
{
    quint64 device = 0; //!< st_dev, or the volume serial number
    quint64 file = 0;   //!< st_ino, or the file index
    qint64 size = 0;
    qint64 mtimeMs = 0; //!< last modification, ms since the epoch
    bool directory = false;
    bool link = false; //!< a symbolic link, a junction or another reparse point

    //! The identity of @p path itself, a link never followed.
    static std::optional<NativeFileIdentity> of(const QString &path);
    static std::optional<NativeFileIdentity> fromJson(const QString &json);
    QString toJson() const;

    //! The same file: what a rename on its file system keeps.
    bool sameFile(const NativeFileIdentity &other) const
    {
        return device == other.device && file == other.file;
    }
    //! The same file, not written since: same size and modification time too.
    bool unchanged(const NativeFileIdentity &other) const
    {
        return sameFile(other) && size == other.size && mtimeMs == other.mtimeMs
            && directory == other.directory && link == other.link;
    }
};

#endif // UTILS_NATIVEFILEIDENTITY_H
