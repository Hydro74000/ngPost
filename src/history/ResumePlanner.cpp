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

namespace
{
QStringList splitStoredGroups(const QString &groups)
{
    return groups.split(',', Qt::SkipEmptyParts);
}
} // namespace

ResumePlanner::JobPlan ResumePlanner::buildJobPlan(const PostHistoryStore::PostDetails &details)
{
    JobPlan plan;
    const int fileRows = static_cast<int>(details.files.size());
    const int originalTotalFiles = details.post.nbFiles > fileRows
        ? details.post.nbFiles
        : fileRows;
    const QStringList postGroups = splitStoredGroups(details.post.groups);

    for (const PostHistoryStore::FileSummary &file : details.files) {
        QSet<uint> postedParts;
        bool hasRemaining = false;
        const QList<PostHistoryStore::ArticleSummary> articles =
            details.articlesByFile.value(file.id);
        for (const PostHistoryStore::ArticleSummary &article : articles) {
            if (article.status == QStringLiteral("posted"))
                postedParts.insert(static_cast<uint>(article.part));
            else
                hasRemaining = true;
        }
        if (file.totalArticles > articles.size())
            hasRemaining = true;
        if (!hasRemaining)
            continue;

        QFileInfo source(file.originalPath);
        if (!source.exists()
            || source.size() != file.sizeBytes
            || (file.mtimeEpoch
                && source.lastModified().toSecsSinceEpoch() != file.mtimeEpoch)) {
            plan.unavailableSources << file.originalPath;
            continue;
        }

        PostingJobResumeFileState state;
        state.historyFileId = file.id;
        state.ordinal = file.ordinal;
        state.totalFiles = originalTotalFiles;
        state.groups = splitStoredGroups(file.groups);
        if (state.groups.isEmpty())
            state.groups = postGroups;
        state.postedParts = postedParts;

        plan.files << source;
        plan.statesByPath.insert(source.absoluteFilePath(), state);
    }

    return plan;
}

PostingJobOptions ResumePlanner::jobOptions(PostingJobOptions base,
                                            const PostHistoryStore::PostDetails &details,
                                            const QString &nzbPath,
                                            const QList<QString> &groups,
                                            const JobPlan &plan,
                                            const std::string &fallbackFrom)
{
    base.nzbFilePath = nzbPath;
    base.files       = plan.files;
    base.grpList     = groups;
    base.from        = details.from.isEmpty() ? fallbackFrom : details.from.toStdString();

    // settings of the original post, not the current globals
    base.obfuscateArticles = details.obfuscateArticles;
    base.obfuscateFileName = details.obfuscateFileName;

    // orders: the archive and the par2 volumes are already on disk
    base.doCompress = false;
    base.doPar2     = false;
    base.rarName    = details.rarName;
    base.rarPass    = details.rarPass;
    base.keepRar    = false;

    base.delFilesAfterPost = false;
    base.overwriteNzb      = true;

    base.resumeHistoryPostId    = details.post.id;
    base.resumeFileStatesByPath = plan.statesByPath;

    // facts about what the original post did, to describe it later
    base.originalDidCompress = details.doCompress;
    base.originalDidPar2     = details.doPar2;

    return base;
}
