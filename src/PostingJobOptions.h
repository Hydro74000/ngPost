// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// Everything a PostingJob needs to know at construction time.
//
// Passing an immutable bundle instead of 25 positional parameters is not only
// about readability: a job may sit in the pending queue for a long time, and
// the global settings of NgPost can change in between. Freezing the options
// when the job is created is what guarantees that a queued post is sent with
// the settings it was queued with.
//
//========================================================================

#ifndef POSTINGJOBOPTIONS_H
#define POSTINGJOBOPTIONS_H

#include "postinfo/PostInfoData.h"

#include <QFileInfo>
#include <QFileInfoList>
#include <QDateTime>
#include <QList>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>

#include <string>

//! Per file resume state, rebuilt from the history by ResumePlanner.
struct PostingJobResumeFileState
{
    qint64      historyFileId = 0;
    int         ordinal       = 0;
    int         totalFiles    = 0;
    QStringList groups;
    QSet<uint>  postedParts;
    //! Size and modification time the file had when it was first posted. Kept
    //! so the source can be checked again just before it is read: a job may
    //! wait a long time in the queue after its plan was built.
    qint64      sizeBytes  = 0;
    qint64      mtimeEpoch = 0;

    //! The one rule deciding whether a source still matches what was posted.
    //! Re-posting parts of a file that changed meanwhile would produce an nzb
    //! nobody can reassemble.
    bool matches(QFileInfo const &file) const
    {
        return file.exists() && file.size() == sizeBytes
               && (mtimeEpoch == 0 || file.lastModified().toSecsSinceEpoch() == mtimeEpoch);
    }
};

struct PostingJobOptions
{
    // what to post, and where the nzb goes
    QString       nzbFilePath;
    QFileInfoList files;

    //! Raw input paths, exactly as the user gave them, BEFORE a folder is
    //! expanded into its files. The caller must fill this: by the time the job
    //! exists, the expansion has already happened. Used for __sourcePath__ and
    //! to refuse overwriting a source file.
    QStringList inputPaths;

    QList<QString> grpList;
    std::string    from;

    bool obfuscateArticles = false;
    bool obfuscateFileName = false;

    // packing
    QString tmpPath;
    QString rarPath;
    QString rarArgs;
    uint    rarSize   = 0;
    bool    useRarMax = false;
    uint    par2Pct   = 0;
    bool    doCompress = false; //!< an ORDER: true means "run rar now"
    bool    doPar2     = false; //!< an ORDER: true means "run par2 now"
    QString rarName;
    QString rarPass;
    bool    keepRar          = false;
    bool    delFilesAfterPost = false;
    bool    overwriteNzb      = true;

    //! User metadata of this post, frozen with the rest of the options. The
    //! password is never in here: it is a secret, see declaredPassword.
    QMap<QString, MetaValue> meta;

    //! Password announced with -m "password=...", when ngPost did not compress
    //! itself and therefore has no rarPass of its own. It is stored, purged and
    //! published exactly like an archive password, never as a metadata.
    QString declaredPassword;

    // resume
    qint64                                       resumeHistoryPostId = 0;
    QMap<QString, PostingJobResumeFileState>      resumeFileStatesByPath;

    //! Facts about the ORIGINAL post, for a resume. They describe what was done
    //! back then; they must never be fed back into doCompress/doPar2, which
    //! would re-run rar and par2 on the leftovers.
    bool originalDidCompress = false;
    bool originalDidPar2     = false;
    int  originalPar2Pct     = -1; //!< < 0 means no par2 at all

    //! par2 percentage to describe in a post info file: the original one for a
    //! resume, the current one otherwise. Never an order.
    int describedPar2Pct() const
    {
        if (resumeHistoryPostId != 0)
            return originalDidPar2 ? originalPar2Pct : -1;
        return doPar2 ? static_cast<int>(par2Pct) : -1;
    }
};

#endif // POSTINGJOBOPTIONS_H
