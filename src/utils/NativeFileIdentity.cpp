// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// Native identity of a file, a link never followed.
//
//========================================================================

#include "utils/NativeFileIdentity.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

std::optional<NativeFileIdentity> NativeFileIdentity::of(const QString &path)
{
    if (path.isEmpty())
        return std::nullopt;
    NativeFileIdentity id;
#if defined(Q_OS_WIN)
    // FILE_FLAG_OPEN_REPARSE_POINT: the junction or link itself, not its target;
    // FILE_FLAG_BACKUP_SEMANTICS: needed to open a folder. Nothing is read.
    const QString native = QDir::toNativeSeparators(path);
    HANDLE handle = ::CreateFileW(reinterpret_cast<const wchar_t *>(native.utf16()),
                                  0,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                  nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return std::nullopt;
    BY_HANDLE_FILE_INFORMATION info = {};
    const BOOL ok = ::GetFileInformationByHandle(handle, &info);
    ::CloseHandle(handle);
    if (!ok)
        return std::nullopt;
    id.device = info.dwVolumeSerialNumber;
    id.file = (static_cast<quint64>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    id.size = static_cast<qint64>((static_cast<quint64>(info.nFileSizeHigh) << 32)
                                  | info.nFileSizeLow);
    // FILETIME counts 100 ns from 1601-01-01; the epoch is 11644473600 s later.
    const quint64 ticks = (static_cast<quint64>(info.ftLastWriteTime.dwHighDateTime) << 32)
        | info.ftLastWriteTime.dwLowDateTime;
    id.mtimeMs = static_cast<qint64>(ticks / 10000) - Q_INT64_C(11644473600000);
    id.directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    id.link = (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    struct stat st = {};
    if (::lstat(QFile::encodeName(path).constData(), &st) != 0)
        return std::nullopt;
    id.device = static_cast<quint64>(st.st_dev);
    id.file = static_cast<quint64>(st.st_ino);
    id.size = static_cast<qint64>(st.st_size);
#if defined(Q_OS_DARWIN)
    id.mtimeMs = static_cast<qint64>(st.st_mtimespec.tv_sec) * 1000
        + st.st_mtimespec.tv_nsec / 1000000;
#else
    id.mtimeMs = static_cast<qint64>(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1000000;
#endif
    id.directory = S_ISDIR(st.st_mode);
    id.link = S_ISLNK(st.st_mode);
#endif
    return id;
}

QString NativeFileIdentity::toJson() const
{
    // 64-bit values as strings: a JSON number is a double and would round them.
    const QJsonObject object{ { QStringLiteral("device"), QString::number(device) },
                              { QStringLiteral("file"), QString::number(file) },
                              { QStringLiteral("size"), QString::number(size) },
                              { QStringLiteral("mtimeMs"), QString::number(mtimeMs) },
                              { QStringLiteral("directory"), directory },
                              { QStringLiteral("link"), link } };
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

std::optional<NativeFileIdentity> NativeFileIdentity::fromJson(const QString &json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isObject())
        return std::nullopt;
    const QJsonObject object = doc.object();
    bool deviceOk = false;
    bool fileOk = false;
    bool sizeOk = false;
    bool mtimeOk = false;
    NativeFileIdentity id;
    id.device = object.value(QStringLiteral("device")).toString().toULongLong(&deviceOk);
    id.file = object.value(QStringLiteral("file")).toString().toULongLong(&fileOk);
    id.size = object.value(QStringLiteral("size")).toString().toLongLong(&sizeOk);
    id.mtimeMs = object.value(QStringLiteral("mtimeMs")).toString().toLongLong(&mtimeOk);
    id.directory = object.value(QStringLiteral("directory")).toBool();
    id.link = object.value(QStringLiteral("link")).toBool();
    if (!deviceOk || !fileOk || !sizeOk || !mtimeOk)
        return std::nullopt;
    return id;
}
