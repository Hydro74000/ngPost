// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// What a PostingJob run by the queue is told about itself, and what its
// preparation leaves behind for the transfer (plan_queue.md §3.4, lot C3).
//
//========================================================================

#ifndef POSTINGJOBEXECUTION_H
#define POSTINGJOBEXECUTION_H

#include "queue/QueueTypes.h"

#include <QFileInfo>
#include <QString>
#include <QStringList>

//! The identity of a queued post a runtime works for. Giving a post id is not
//! a resume: only PostingJobOptions::resumeHistoryPostId means that.
struct JobExecutionContext
{
    QString ownerId;
    qint64 postId = 0;
    QString runId;
    queue::Origin origin = queue::Origin::Quick;
    int tabNumber = 0;
    QString setId; //!< artifact set being prepared or posted, if any
};

//! The files a preparation produced, as the transfer will publish them. Kept
//! in memory in lot C3; lot C5 writes it with its artifact set.
struct PreparedManifest
{
    QString dir;            //!< folder holding the prepared files
    QStringList files;      //!< absolute paths, in publication order
    QStringList copiedNfos; //!< .nfo copies among them: not part of the post size
    qint64 bytes = 0;       //!< temporary space they take, for the budget
    bool compressed = false;
    bool par2 = false;

    //! Every file still there, and something prepared among them: an archive
    //! publishes only what is inside dir, PAR2 alone adds its volumes to the
    //! original files.
    bool isValid() const
    {
        if (dir.isEmpty() || files.isEmpty())
            return false;
        const QString root = QFileInfo(dir).absoluteFilePath() + QLatin1Char('/');
        bool prepared = false;
        for (const QString &path : files) {
            const QFileInfo file(path);
            if (!file.isFile())
                return false;
            const bool inside = file.absoluteFilePath().startsWith(root);
            if (compressed && !inside)
                return false;
            prepared = prepared || inside;
        }
        return prepared;
    }
};

#endif // POSTINGJOBEXECUTION_H
