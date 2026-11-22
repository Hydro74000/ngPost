// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>

#include "QueuePreview.h"

#include <QFileInfo>

namespace queue
{

namespace
{

QueueScheduler::Budget previewBudget()
{
    QueueScheduler::Budget budget;
    budget.preparedMax = 2;
    return budget;
}

} // namespace

QueuePreview::QueuePreview(QObject *parent)
    : QObject(parent)
    , _queue(&_store, QStringLiteral("preview"))
    , _executor()
    , _scheduler(&_queue, &_executor, previewBudget())
{
    _executor.setSimulation(true, 2500, 20000);
}

bool QueuePreview::requested()
{
    return qEnvironmentVariableIsSet("NGPOST_QUEUE_PREVIEW");
}

void QueuePreview::add(const QString &name,
                       bool compress,
                       bool par2,
                       qint64 bytes,
                       Origin origin,
                       int tab)
{
    PersistedQueueOptions o;
    o.job.nzbFilePath = QStringLiteral("/preview/%1.nzb").arg(name);
    o.job.grpList = { QStringLiteral("alt.binaries.test") };
    o.job.articleSizeBytes = 716800;
    o.job.doCompress = compress;
    o.job.doPar2 = par2;
    o.job.par2Pct = 8;
    o.archiverFamily = compress ? QStringLiteral("rar") : QString();
    o.par2Family = par2 ? QStringLiteral("par2cmdline") : QString();
    PersistedSource source;
    source.path = QStringLiteral("/preview/%1.iso").arg(name);
    source.sizeBytes = bytes;
    o.sources << source;
    o.job.files << QFileInfo(source.path);
    _queue.submit(o, origin, tab);
}

void QueuePreview::addDemoPosts()
{
    constexpr qint64 GB = 1024LL * 1024 * 1024;
    add(QStringLiteral("Linux_Mint_22_Cinnamon_x64"), false, false, 3 * GB, Origin::Quick, 2);
    add(QStringLiteral("Debian_13_Netinst"), true, true, GB / 2, Origin::Auto, 0);
    add(QStringLiteral("Archlinux-2026.11.01-x86_64"), true, false, GB, Origin::Auto, 0);
    add(QStringLiteral("Fedora-Workstation-Live-43"), true, true, 2 * GB, Origin::Quick, 3);
    add(QStringLiteral("Ubuntu-26.04-Desktop-amd64"), true, true, 5 * GB, Origin::Auto, 0);
    add(QStringLiteral("FreeBSD_15_Release_amd64"), false, false, 2 * GB, Origin::Resume, 0);
    add(QStringLiteral("openSUSE-Tumbleweed-DVD"), true, false, 4 * GB, Origin::Drop, 0);
}

void QueuePreview::addPaths(const QStringList &paths)
{
    for (const QString &path : paths) {
        const QFileInfo info(path);
        add(info.completeBaseName(), true, true, info.isFile() ? info.size() : 0, Origin::Drop, 0);
    }
}

} // namespace queue
