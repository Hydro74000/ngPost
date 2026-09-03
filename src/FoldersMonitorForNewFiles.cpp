//========================================================================
//
// Copyright (C) 2020 Matthieu Bruel <Matthieu.Bruel@gmail.com>
// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3..
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>
//
//========================================================================

#include "FoldersMonitorForNewFiles.h"
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QMutexLocker>
#include <QThread>
#include "utils/Macros.h"

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

ulong FoldersMonitorForNewFiles::sMSleep
    = 1000; //!< 1sec in case we move file from samba or unrar when the system is quite loaded
ushort FoldersMonitorForNewFiles::sNbStableScans = 2;
#if defined(Q_OS_WIN)
ushort FoldersMonitorForNewFiles::sMaxLockRetries = 30;
#endif

FoldersMonitorForNewFiles::FoldersMonitorForNewFiles(const QString &folderPath, QObject *parent)
    : QObject(parent)
    , _monitor()
    , _stopListening(0x0)
{
    connect(&_monitor,
            &QFileSystemWatcher::directoryChanged,
            this,
            &FoldersMonitorForNewFiles::onDirectoryChanged);
    addFolder(folderPath);
}

FoldersMonitorForNewFiles::~FoldersMonitorForNewFiles()
{
    qDeleteAll(_folders);
}

bool FoldersMonitorForNewFiles::addFolder(const QString &folderPath)
{
    QFileInfo fi(folderPath);
    if (fi.exists() && fi.isDir() && fi.isReadable()) {
        qDebug() << "monitoring new folder: " << folderPath;
        _folders.insert(folderPath, new FolderScan(folderPath));
        _monitor.addPath(folderPath);
        return true;
    } else
        return false;
}

void FoldersMonitorForNewFiles::stopListening()
{
    qDebug() << "[FoldersMonitorForNewFiles::stopListening] stop monitoring!";

    _stopListening.testAndSetOrdered(0x0, 0x1);
    disconnect(&_monitor,
               &QFileSystemWatcher::directoryChanged,
               this,
               &FoldersMonitorForNewFiles::onDirectoryChanged);
}

void FoldersMonitorForNewFiles::onDirectoryChanged(const QString &folderPath)
{
    if (MB_LoadAtomic(_stopListening))
        return;

    FolderScan *folderScan = _folders[folderPath];
    QDateTime currentUpdate = QFileInfo(folderPath).lastModified();

    qDebug() << "[directoryChanged] " << folderPath << " (lastUpdate: " << folderScan->lastUpdate
             << ", now: " << currentUpdate << ")";

#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
    PathSet newScan
        = QDir(folderPath).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).toSet();
#else
    QStringList newScanTmpList = QDir(folderPath)
                                     .entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    PathSet newScan(newScanTmpList.begin(), newScanTmpList.end());
#endif

    // iterate new paths
    PathSet newFiles = newScan; // this will detach!
    newFiles.subtract(folderScan->previousScan);
    for (const QString &fileName : newFiles) {
        if (MB_LoadAtomic(_stopListening))
            break;

        QString filePath = QString("%1/%2").arg(folderPath).arg(fileName);
        QFileInfo fi(filePath);
        fi.setCaching(
            false); // really important to be able to check if the file is finished to be written

        // Did we put this here ourselves? Checked before anything else so an
        // obfuscation rename never costs a full stability wait. It lands in
        // previousScan at the end of the sweep, so it is seen exactly once.
        if (_consumeIgnoredPath(_normalized(fi.absoluteFilePath()))) {
            qDebug() << "[directoryChanged] ignoring path ngPost renamed itself: " << filePath;
            continue;
        }

        if (!fi.exists()) {
            qCritical() << "[directoryChanged] error file doesn't exist: " << filePath;
            continue;
        }

        qint64 size = _pathSize(fi);
        qDebug() << "[directoryChanged] processing new file: " << filePath << ", size: " << size
                 << ", lastModif: " << fi.lastModified();

        ushort nbWait = _waitUntilFullyWritten(fi, size);

#if defined(Q_OS_WIN)
        // Windows is the only platform where "is another process still writing
        // this?" has an answer, and the size going quiet is not it (issue #112).
        if (fi.exists() && !fi.isDir() && !_waitUntilNotLocked(fi, nbWait))
            qDebug() << "[directoryChanged] WARNING: still locked by another process after "
                     << nbWait * sMSleep << " msec, processing anyway: " << filePath;
#endif

        if (fi.exists()) {
            qDebug() << "[directoryChanged] after " << nbWait * sMSleep << " msec, "
                     << "ready to process file: " << filePath << ", size: " << size
                     << ", lastModif: " << fi.lastModified();

            if (!MB_LoadAtomic(_stopListening))
                emit newFileToProcess(fi);
#ifdef __DEBUG__
            if (fi.isDir()) {
                for (QFileInfo &subFile :
                     QDir(fi.absoluteFilePath())
                         .entryInfoList(QDir::Files | QDir::Hidden | QDir::System | QDir::Dirs
                                        | QDir::NoDotAndDotDot))
                    qDebug() << "\t- " << subFile.fileName() << ": size: " << _pathSize(subFile);
            }
#endif
        } else
            qDebug() << "[directoryChanged] ignoring temporary file: " << filePath;
    }

    // Anything still reserved for this folder that the scan just saw is now
    // part of the baseline below, so it can never be reported as new again and
    // the reservation is spent. Dropping it matters: the watcher coalesces
    // events, and a file put back before any scan noticed it had left is never
    // "new" -- a reservation left behind would silently swallow a genuine
    // re-drop of the same name later on.
    _releaseSpentReservations(folderPath, newScan);

    folderScan->lastUpdate = currentUpdate;
    folderScan->previousScan = newScan;
}

void FoldersMonitorForNewFiles::ignoreNextAppearance(const QString &path)
{
    QMutexLocker lock(&_ignoredPathsMutex);
    _ignoredPaths.insert(_normalized(path));
}

void FoldersMonitorForNewFiles::stopIgnoringMonitorPath(const QString &path)
{
    QMutexLocker lock(&_ignoredPathsMutex);
    _ignoredPaths.remove(_normalized(path));
}

void FoldersMonitorForNewFiles::_releaseSpentReservations(const QString &folderPath,
                                                          const PathSet &scan)
{
    QMutexLocker lock(&_ignoredPathsMutex);
    if (_ignoredPaths.isEmpty())
        return;

    const QString prefix = _normalized(folderPath) + QLatin1Char('/');
    for (auto it = _ignoredPaths.begin(); it != _ignoredPaths.end();) {
        const QString &entry = *it;
        if (!entry.startsWith(prefix)) {
            ++it;
            continue;
        }
        // Direct children only: the scan lists this folder, not its subtrees.
        const QString name = entry.mid(prefix.size());
        if (name.contains(QLatin1Char('/')) || !scan.contains(name))
            ++it;
        else
            it = _ignoredPaths.erase(it);
    }
}

QString FoldersMonitorForNewFiles::_normalized(const QString &path)
{
    // absoluteFilePath() rather than canonicalFilePath(): the latter returns an
    // empty string for a path that does not exist yet, and we register paths
    // precisely before creating them.
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool FoldersMonitorForNewFiles::_consumeIgnoredPath(const QString &absolutePath)
{
    QMutexLocker lock(&_ignoredPathsMutex);
    return _ignoredPaths.remove(absolutePath);
}

//! Block until the path stops changing, and report how many intervals it took.
//! \a size is left holding the settled size.
ushort FoldersMonitorForNewFiles::_waitUntilFullyWritten(QFileInfo &fileInfo, qint64 &size) const
{
    ushort nbWait = 0, nbStable = 0;
    qint64 lastSize = _pathSize(fileInfo);
    QDateTime lastModified = fileInfo.lastModified();

    while (fileInfo.exists() && nbStable < sNbStableScans) {
        QThread::msleep(sMSleep);
        ++nbWait;

        qint64 const newSize = _pathSize(fileInfo);
        QDateTime const newModified = fileInfo.lastModified();
        if (newSize == lastSize && newModified == lastModified)
            ++nbStable;
        else
            nbStable = 0; // it moved again: start counting over

        lastSize = newSize;
        lastModified = newModified;
    }

    size = lastSize;
    return nbWait;
}

#if defined(Q_OS_WIN)
bool FoldersMonitorForNewFiles::_waitUntilNotLocked(const QFileInfo &fileInfo, ushort &nbWait) const
{
    // The question is "does anyone hold this open for writing?", and
    // dwShareMode = FILE_SHARE_READ asks exactly that: we tolerate other
    // readers (an antivirus or a search indexer must not read as a lock), but
    // the open fails with ERROR_SHARING_VIOLATION for as long as the copying
    // process holds its write handle. GENERIC_READ is all we request, so this
    // also works on a read-only share -- opening for writing would report
    // "locked" forever on any file we simply are not allowed to write.
    for (ushort nbRetries = 0; nbRetries < sMaxLockRetries; ++nbRetries) {
        QString const native = QDir::toNativeSeparators(fileInfo.absoluteFilePath());
        HANDLE handle = ::CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()),
                                      GENERIC_READ,
                                      FILE_SHARE_READ,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle);
            return true;
        }
        if (::GetLastError() != ERROR_SHARING_VIOLATION)
            return true; // gone, or denied for another reason: not ours to solve

        QThread::msleep(sMSleep);
        ++nbWait;
    }
    return false;
}
#endif

qint64 FoldersMonitorForNewFiles::_pathSize(QFileInfo &fileInfo) const
{
    fileInfo.refresh();
    if (fileInfo.isDir())
        return _dirSize(fileInfo.absoluteFilePath());
    else
        return fileInfo.size();
}

qint64 FoldersMonitorForNewFiles::_dirSize(const QString &path) const
{
    qint64 size = 0;
    QDir dir(path);
    for (const QFileInfo &fi : dir.entryInfoList(QDir::Files | QDir::Hidden | QDir::System
                                                 | QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (fi.isDir())
            size += _dirSize(fi.absoluteFilePath());
        else
            size += fi.size();
    }
    return size;
}

FolderScan::FolderScan(const QString &folderPath)
    : path(folderPath)
    , lastUpdate(QDateTime::currentDateTime())
    ,
#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
    previousScan(QDir(folderPath).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).toSet())
#else
    previousScan()
#endif
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    QStringList scanList = QDir(folderPath)
                               .entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    previousScan = PathSet(scanList.begin(), scanList.end());
#endif
}
