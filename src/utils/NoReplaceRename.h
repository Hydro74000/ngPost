// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// A rename that neither copies nor replaces (plan_queue.md lot C5.7), for the
// sources an obfuscation moves aside and puts back. One atomic system call:
// Linux renameat2(RENAME_NOREPLACE), macOS renamex_np(RENAME_EXCL), Windows
// MoveFileExW without MOVEFILE_REPLACE_EXISTING nor MOVEFILE_COPY_ALLOWED.
// Where the file system offers no such call, nothing is moved: no fallback
// that would leave both paths (link + unlink) or race (check then rename).
//
//========================================================================

#ifndef UTILS_NOREPLACERENAME_H
#define UTILS_NOREPLACERENAME_H

#include <QString>

namespace noreplace
{

enum class Result {
    Renamed,
    Exists,      //!< something is already there: nothing moved
    Unsupported, //!< no atomic no-replace rename on this file system: nothing moved
    Failed       //!< any other refusal (another file system, rights...): see the error
};

//! Renames @p from to @p to. @p error gets errno, or the Win32 error.
Result rename(const QString &from, const QString &to, int *error = nullptr);

//! Makes the renames done in folder @p dir durable (fsync of the folder).
//! Windows renames are written through already.
bool syncDirectory(const QString &dir);

} // namespace noreplace

#endif // UTILS_NOREPLACERENAME_H
