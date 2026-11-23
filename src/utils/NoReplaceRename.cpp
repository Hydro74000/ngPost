// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// A rename that neither copies nor replaces: see NoReplaceRename.h.
//
//========================================================================

#include "utils/NoReplaceRename.h"

#include <QDir>
#include <QFile>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace noreplace
{

Result rename(const QString &from, const QString &to, int *error)
{
    int code = 0;
    Result result = Result::Renamed;
#ifdef NGPOST_TESTING
    // Stands for a file system without any atomic no-replace rename.
    if (qEnvironmentVariableIsSet("NGPOST_TEST_NO_ATOMIC_RENAME")) {
        if (error)
            *error = 0;
        return Result::Unsupported;
    }
#endif
#if defined(Q_OS_WIN)
    const QString source = QDir::toNativeSeparators(from);
    const QString target = QDir::toNativeSeparators(to);
    if (!::MoveFileExW(reinterpret_cast<const wchar_t *>(source.utf16()),
                       reinterpret_cast<const wchar_t *>(target.utf16()),
                       MOVEFILE_WRITE_THROUGH)) {
        code = static_cast<int>(::GetLastError());
        result = code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS ? Result::Exists
                                                                           : Result::Failed;
    }
#else
    const QByteArray source = QFile::encodeName(from);
    const QByteArray target = QFile::encodeName(to);
#if defined(Q_OS_LINUX)
    const int done = ::renameat2(AT_FDCWD,
                                 source.constData(),
                                 AT_FDCWD,
                                 target.constData(),
                                 RENAME_NOREPLACE);
#elif defined(Q_OS_DARWIN)
    const int done = ::renamex_np(source.constData(), target.constData(), RENAME_EXCL);
#else
    // No atomic no-replace rename known here: nothing is moved.
    const int done = -1;
    errno = ENOTSUP;
#endif
    if (done != 0) {
        code = errno;
        if (code == EEXIST || code == ENOTEMPTY)
            result = Result::Exists;
        else if (code == EINVAL || code == ENOSYS || code == ENOTSUP || code == EOPNOTSUPP)
            result = Result::Unsupported;
        else
            result = Result::Failed;
    }
#endif
    if (error)
        *error = code;
    return result;
}

bool syncDirectory(const QString &dir)
{
#ifdef NGPOST_TESTING
    // Stands for a folder whose changes cannot be made durable.
    if (qEnvironmentVariableIsSet("NGPOST_TEST_FAIL_DIR_SYNC"))
        return false;
#endif
#if defined(Q_OS_WIN)
    Q_UNUSED(dir)
    return true; // MOVEFILE_WRITE_THROUGH
#else
    const int fd = ::open(QFile::encodeName(dir).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool synced = ::fsync(fd) == 0;
    ::close(fd);
    return synced;
#endif
}

} // namespace noreplace
