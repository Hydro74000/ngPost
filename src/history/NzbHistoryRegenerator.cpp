// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// NZB regeneration from structured history.
//
//========================================================================

#include "history/NzbHistoryRegenerator.h"

#include "NgPost.h"

#include <QDateTime>
#include <QTextStream>

#include <algorithm>

namespace
{

QString escapeXml(QString s)
{
    return NgPost::escapeXML(s);
}

QStringList splitGroups(const QString &groups)
{
    return groups.split(',', Qt::SkipEmptyParts);
}

qint64 ceilDivide(qint64 numerator, qint64 denominator)
{
    if (numerator <= 0 || denominator <= 0)
        return 0;
    return numerator / denominator + (numerator % denominator ? 1 : 0);
}

bool isPlausibleFullArticleSize(const PostHistoryStore::FileSummary &file,
                                qint64 fullArticleBytes)
{
    if (file.totalArticles <= 0 || file.sizeBytes <= 0 || fullArticleBytes <= 0)
        return false;
    if (fullArticleBytes < ceilDivide(file.sizeBytes, file.totalArticles))
        return false;
    if (file.totalArticles == 1)
        return file.sizeBytes <= fullArticleBytes;
    return file.sizeBytes > fullArticleBytes * static_cast<qint64>(file.totalArticles - 1);
}

qint64 inferFullArticleBytes(const PostHistoryStore::FileSummary &file,
                             const QList<PostHistoryStore::ArticleSummary> &articles,
                             qint64 postFullArticleBytesHint,
                             bool postHintIsExact)
{
    // Since schema v3 this is the boundary the original PostingJob actually
    // used. It must win over today's global ARTICLE_SIZE, even when both sizes
    // happen to be mathematically plausible for the same article count.
    if (postHintIsExact)
        return postFullArticleBytesHint;

    qint64 historyMaxBytes = 0;
    for (const PostHistoryStore::ArticleSummary &article : articles) {
        if (article.bytes > historyMaxBytes)
            historyMaxBytes = article.bytes;
    }

    qint64 fullArticleBytes =
        std::max(historyMaxBytes, ceilDivide(file.sizeBytes, file.totalArticles));
    if (isPlausibleFullArticleSize(file, postFullArticleBytesHint)
        && postFullArticleBytesHint > fullArticleBytes)
        fullArticleBytes = postFullArticleBytesHint;
    const qint64 configuredArticleBytes = NgPost::articleSize();
    if (isPlausibleFullArticleSize(file, configuredArticleBytes)
        && configuredArticleBytes > fullArticleBytes)
        fullArticleBytes = configuredArticleBytes;
    if (fullArticleBytes <= 0)
        fullArticleBytes = configuredArticleBytes;
    return fullArticleBytes;
}

qint64 inferPostFullArticleBytesHint(const PostHistoryStore::PostDetails &details)
{
    qint64 maxBytes = 0;
    for (const PostHistoryStore::FileSummary &file : details.files) {
        const QList<PostHistoryStore::ArticleSummary> articles =
            details.articlesByFile.value(file.id);
        for (const PostHistoryStore::ArticleSummary &article : articles) {
            if (article.bytes > maxBytes)
                maxBytes = article.bytes;
        }
    }
    return maxBytes;
}

qint64 inferSegmentBytes(const PostHistoryStore::FileSummary &file,
                         int part,
                         qint64 fullArticleBytes)
{
    if (file.totalArticles <= 0)
        return fullArticleBytes;
    if (part == file.totalArticles && file.sizeBytes > 0) {
        if (file.totalArticles == 1)
            return file.sizeBytes;
        const qint64 lastBytes =
            file.sizeBytes - fullArticleBytes * static_cast<qint64>(file.totalArticles - 1);
        if (lastBytes > 0)
            return lastBytes;
    }
    return fullArticleBytes;
}

} // namespace

NzbHistoryRegenerator::NzbHistoryRegenerator(PostHistoryStore *store)
    : _store(store)
{
}

bool NzbHistoryRegenerator::writeNzb(qint64 postId,
                                     QTextStream &stream,
                                     bool includePassword,
                                     QStringList *warnings,
                                     QString *error,
                                     const QString &passwordOverride)
{
    if (!_store) {
        if (error)
            *error = tr("history store is not available");
        return false;
    }

    PostHistoryStore::PostDetails details;
    if (!_store->loadPostDetails(postId, &details, error))
        return false;

    if (details.post.status == QStringLiteral("partial") && warnings)
        *warnings << tr("post is partial; regenerated NZB may be incomplete");
    if (details.files.isEmpty()) {
        if (error)
            *error = tr("the post has no structured file/article history; refusing to create an "
                        "empty NZB");
        return false;
    }
    if (details.post.hasPassword && !details.post.passwordStored
        && passwordOverride.isEmpty() && warnings)
        *warnings << tr("post had an archive password, but it is not stored");

    const QString tab = NgPost::space();
    stream << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           << "<!DOCTYPE nzb PUBLIC \"-//newzBin//DTD NZB 1.1//EN\" "
              "\"http://www.newzbin.com/DTD/nzb/nzb-1.1.dtd\">\n"
           << "<nzb xmlns=\"http://www.newzbin.com/DTD/2003/nzb\">\n";

    // Metadata the user chose to publish. This file is the one that lands on
    // disk (the streamed nzb is rewritten from here), so anything missing in
    // this block is missing from the delivered nzb.
    QMap<QString, MetaValue> publishedMeta;
    {
        PostHistoryStore::PostInfoRecord record;
        QString metaError;
        if (_store->loadPostInfoRecord(postId, &record, &metaError)) {
            for (auto it = record.meta.cbegin(); it != record.meta.cend(); ++it) {
                if (it.value().scope == MetaScope::Nzb)
                    publishedMeta.insert(it.key(), it.value());
            }
        } else {
            if (error)
                *error = tr("could not read the metadata of the post: %1").arg(metaError);
            return false;
        }
    }

    // Automatic finalisation still owns the live password even when the user
    // chose HISTORY_STORE_PASSWORDS=false. Use that one-time overlay so
    // rewriting the streamed NZB from history does not silently strip its
    // <meta type="password">. Historical exports deliberately pass no
    // override and therefore remain empty unless storage was enabled.
    const QString effectivePassword = passwordOverride.isEmpty()
        ? details.rarPass
        : passwordOverride;
    const bool passwordAvailable = !passwordOverride.isEmpty()
        || (details.post.passwordStored && !details.rarPass.isEmpty());
    const bool writePassword = includePassword && passwordAvailable;
    if (writePassword || !publishedMeta.isEmpty()) {
        stream << tab << "<head>\n";
        for (auto it = publishedMeta.cbegin(); it != publishedMeta.cend(); ++it) {
            stream << tab << tab << "<meta type=\"" << escapeXml(it.key()) << "\">"
                   << escapeXml(it.value().value) << "</meta>\n";
        }
        if (writePassword) {
            stream << tab << tab << "<meta type=\"password\">" << escapeXml(effectivePassword)
                   << "</meta>\n";
        }
        stream << tab << "</head>\n\n";
    }

    int padding = 1;
    int n = details.files.size();
    while (n >= 10) {
        ++padding;
        n /= 10;
    }

    if (details.articleSizeWasStored && details.articleSizeBytes <= 0) {
        if (error)
            *error = tr("the stored article size is inconsistent with the post history; refusing "
                        "to rebuild the NZB");
        return false;
    }
    const bool hasExactArticleSize = details.articleSizeBytes > 0;
    const qint64 postFullArticleBytesHint = hasExactArticleSize
        ? details.articleSizeBytes
        : inferPostFullArticleBytesHint(details);
    // Segment sizes have to describe the same thing throughout one nzb. A post
    // that straddles the v3 -> v4 upgrade has body_bytes on the articles posted
    // after it and nothing on the ones before, so mixing the two would hand the
    // client a document where some segments are yEnc encoded sizes and others
    // are the decoded ones. When even a single article predates the column,
    // fall back to the decoded sizes everywhere -- the behaviour of every
    // release before v4.
    bool useBodyBytes = true;
    for (const PostHistoryStore::FileSummary &file : details.files) {
        for (const PostHistoryStore::ArticleSummary &article : details.articlesByFile.value(file.id)) {
            if (article.status == QStringLiteral("posted") && !article.msgId.isEmpty()
                && article.bodyBytes <= 0) {
                useBodyBytes = false;
                break;
            }
        }
        if (!useBodyBytes)
            break;
    }

    int repairedArticleBytes = 0;
    for (const PostHistoryStore::FileSummary &file : details.files) {
        const QList<PostHistoryStore::ArticleSummary> articles = details.articlesByFile.value(file.id);
        const qint64 fullArticleBytes =
            inferFullArticleBytes(file, articles, postFullArticleBytesHint, hasExactArticleSize);
        bool hasUnknown = false;
        bool hasNonPosted = false;
        bool hasMissing = articles.size() < file.totalArticles;
        for (const PostHistoryStore::ArticleSummary &article : articles) {
            if (article.status == QStringLiteral("unknown"))
                hasUnknown = true;
            if (article.status != QStringLiteral("posted"))
                hasNonPosted = true;
            if (article.status == QStringLiteral("posted") && article.msgId.isEmpty())
                hasMissing = true;
        }
        if (hasUnknown && warnings)
            *warnings << tr("file %1 contains unknown articles").arg(file.postedName);
        if (hasNonPosted && warnings)
            *warnings << tr("file %1 contains non-posted articles").arg(file.postedName);
        if (hasMissing && warnings)
            *warnings << tr("file %1 has missing article records").arg(file.postedName);
        if (details.post.status == QStringLiteral("success")
            && (hasNonPosted || hasMissing)) {
            if (error)
                *error = tr("history for successful file %1 is incomplete; refusing to replace "
                            "the NZB")
                             .arg(file.postedName);
            return false;
        }

        stream << tab << "<file poster=\"" << escapeXml(details.from) << "\""
               << " date=\"" << QDateTime::currentSecsSinceEpoch() << "\""
               << QString(" subject=\"[%1/%2] - &quot;")
                      .arg(file.ordinal, padding, 10, QChar('0'))
                      .arg(details.files.size())
               << escapeXml(file.postedName)
               << "&quot; yEnc (1/" << file.totalArticles << ") " << file.sizeBytes << "\">\n";

        stream << tab << tab << "<groups>\n";
        for (const QString &group : splitGroups(file.groups))
            stream << tab << tab << tab << "<group>" << escapeXml(group) << "</group>\n";
        stream << tab << tab << "</groups>\n";

        stream << tab << tab << "<segments>\n";
        for (const PostHistoryStore::ArticleSummary &article : articles) {
            if (article.status != QStringLiteral("posted") || article.msgId.isEmpty())
                continue;
            // What the nzb advertises is the size of the article ON THE SERVER,
            // i.e. the yEnc encoded body -- roughly 3% larger than the data it
            // decodes to. `bytes` is that decoded slice, kept for rows written
            // before body_bytes existed (schema v4).
            qint64 segmentBytes = useBodyBytes ? article.bodyBytes : article.bytes;
            if (segmentBytes <= 0) {
                segmentBytes = inferSegmentBytes(file, article.part, fullArticleBytes);
                ++repairedArticleBytes;
            }
            stream << tab << tab << tab << "<segment"
                   << " bytes=\"" << segmentBytes << "\""
                   << " number=\"" << article.part << "\">"
                   << escapeXml(article.msgId)
                   << "</segment>\n";
        }
        stream << tab << tab << "</segments>\n"
               << tab << "</file>\n";
    }

    if (repairedArticleBytes > 0 && warnings)
        *warnings << tr("%1 article segment sizes were missing in history and rebuilt from file metadata")
                         .arg(repairedArticleBytes);

    stream << "</nzb>\n";
    return true;
}
