// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// Resume candidate classification.
//
//========================================================================

#include "history/ResumePlanner.h"

#include <QDateTime>
#include <QFileInfo>

ResumePlanner::ResumePlanner(PostHistoryStore *store)
    : _store(store)
{
}

ResumePlanner::Decision ResumePlanner::check(qint64 postId, QString *error)
{
    Decision decision;
    decision.postId = postId;
    if (!_store) {
        if (error)
            *error = tr("history store is not available");
        decision.reason = tr("history store is not available");
        return decision;
    }

    PostHistoryStore::PostDetails details;
    if (!_store->loadPostDetails(postId, &details, error)) {
        decision.reason = error ? *error : tr("cannot load post");
        return decision;
    }

    if (details.files.isEmpty()) {
        decision.state = ResumeState::NotResumable;
        decision.reason = tr("posting never started; nothing to resume");
        return decision;
    }

    bool missingSource = false;
    for (const PostHistoryStore::FileSummary &file : details.files) {
        if (file.originalPath.isEmpty())
            missingSource = true;
        else {
            QFileInfo fi(file.originalPath);
            if (!fi.exists()
                || fi.size() != file.sizeBytes
                || (file.mtimeEpoch && fi.lastModified().toSecsSinceEpoch() != file.mtimeEpoch))
                missingSource = true;
        }
        const QList<PostHistoryStore::ArticleSummary> articles = details.articlesByFile.value(file.id);
        if (file.totalArticles > articles.size())
            decision.pendingArticles += file.totalArticles - articles.size();
        for (const PostHistoryStore::ArticleSummary &article : articles) {
            if (article.status == QStringLiteral("posted"))
                ++decision.postedArticles;
            else if (article.status == QStringLiteral("failed"))
                ++decision.failedArticles;
            else if (article.status == QStringLiteral("unknown"))
                ++decision.unknownArticles;
            else
                ++decision.pendingArticles;
        }
    }

    const int remaining = decision.pendingArticles + decision.failedArticles + decision.unknownArticles;
    if (remaining == 0) {
        decision.state = ResumeState::NotResumable;
        decision.reason = tr("nothing remains to resume");
    } else if (missingSource) {
        decision.state = ResumeState::NotResumable;
        decision.reason = tr("source files are missing or changed");
    } else if (decision.postedArticles > 0) {
        decision.state = ResumeState::PartiallyResumable;
        decision.reason = tr("some articles are already posted");
    } else {
        decision.state = ResumeState::Resumable;
        decision.reason = tr("post can be resumed");
    }
    return decision;
}
