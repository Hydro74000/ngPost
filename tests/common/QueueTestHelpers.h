// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// Header-only helpers shared by the queue unit tests (tst_PostQueue,
// tst_QueueScheduler, tst_PostingJobOptionsCodec), which build only the
// queue sources and do not include common.pri.
//
//========================================================================

#ifndef QUEUETESTHELPERS_H
#define QUEUETESTHELPERS_H

#include "PostingJobOptionsCodec.h"
#include "par2/Par2Settings.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QString>

namespace queuetest
{

//! A recipe that never touches the disk: the paths are only names.
inline PersistedQueueOptions makeOptions(const QString &name,
                                         bool compress = false,
                                         bool par2 = false,
                                         qint64 bytes = 1000)
{
    PersistedQueueOptions o;
    o.job.nzbFilePath = QStringLiteral("/nonexistent/ngpost-queue-tests/%1.nzb").arg(name);
    o.job.grpList = { QStringLiteral("alt.binaries.test") };
    o.job.articleSizeBytes = 716800;
    o.job.doCompress = compress;
    o.job.doPar2 = par2;
    o.job.par2Pct = 10;
    o.job.keepRar = false;
    o.archiverFamily = compress ? QStringLiteral("rar") : QString();
    o.par2Family = par2 ? par2::toolName(par2::Tool::Par2cmdline) : QString();
    PersistedSource source;
    source.path = QStringLiteral("/nonexistent/ngpost-queue-tests/%1.bin").arg(name);
    source.sizeBytes = bytes;
    o.sources << source;
    o.job.files << QFileInfo(source.path);
    return o;
}

//! Runs the queued re-evaluations of the scheduler.
inline void settle()
{
    for (int i = 0; i < 5; ++i)
        QCoreApplication::processEvents();
}

} // namespace queuetest

#endif // QUEUETESTHELPERS_H
