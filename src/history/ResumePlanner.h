// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// Resume candidate classification.
//
//========================================================================

#ifndef RESUMEPLANNER_H
#define RESUMEPLANNER_H

#include "PostingJobOptions.h"
#include "history/PostHistoryStore.h"

#include <QCoreApplication>
#include <QFileInfoList>

#include <string>

class ResumePlanner
{
    Q_DECLARE_TR_FUNCTIONS(ResumePlanner)

public:
    enum class ResumeState {
        NotResumable,
        PartiallyResumable,
        Resumable
    };

    struct Decision {
        qint64 postId = 0;
        ResumeState state = ResumeState::NotResumable;
        QString reason;
        int postedArticles = 0;
        int pendingArticles = 0;
        int failedArticles = 0;
        int unknownArticles = 0;
    };

    //! What is left to re-post, and what can no longer be re-posted.
    struct JobPlan {
        QFileInfoList files;
        QMap<QString, PostingJobResumeFileState> statesByPath;
        QStringList unavailableSources; //!< sources gone, resized or touched since
    };

    explicit ResumePlanner(PostHistoryStore *store);

    Decision check(qint64 postId, QString *error = nullptr);

    //! Files with at least one article left to post, skipping the sources that
    //! no longer match what was posted.
    static JobPlan buildJobPlan(const PostHistoryStore::PostDetails &details);

    //! Options of a resumed post. Two kinds of settings must not be confused:
    //!  - ORDERS (doCompress, doPar2) stay false: the archive and the par2
    //!    volumes already exist on disk, replaying them would rebuild them out
    //!    of the leftovers;
    //!  - SETTINGS of the original post (obfuscation, poster, groups) are
    //!    replayed from the history, because the current globals may well have
    //!    changed since that post was made.
    //! What the original post did is carried separately, as plain facts, for
    //! whoever needs to describe the post rather than redo it.
    static PostingJobOptions jobOptions(PostingJobOptions base,
                                        const PostHistoryStore::PostDetails &details,
                                        const QString &nzbPath,
                                        const QList<QString> &groups,
                                        const JobPlan &plan,
                                        const std::string &fallbackFrom);

private:
    PostHistoryStore *_store;
};

#endif // RESUMEPLANNER_H
