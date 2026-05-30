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
                             qint64 postFullArticleBytesHint)
{
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
                                     QString *error)
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
    if (details.post.hasPassword && !details.post.passwordStored && warnings)
        *warnings << tr("post had an archive password, but it is not stored");

    const QString tab = NgPost::space();
    stream << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           << "<!DOCTYPE nzb PUBLIC \"-//newzBin//DTD NZB 1.1//EN\" "
              "\"http://www.newzbin.com/DTD/nzb/nzb-1.1.dtd\">\n"
           << "<nzb xmlns=\"http://www.newzbin.com/DTD/2003/nzb\">\n";

    if (includePassword && details.post.passwordStored && !details.rarPass.isEmpty()) {
        stream << tab << "<head>\n"
               << tab << tab << "<meta type=\"password\">" << escapeXml(details.rarPass)
               << "</meta>\n"
               << tab << "</head>\n\n";
    }

    int padding = 1;
    int n = details.files.size();
    while (n >= 10) {
        ++padding;
        n /= 10;
    }

    const qint64 postFullArticleBytesHint = inferPostFullArticleBytesHint(details);
    int repairedArticleBytes = 0;
    for (const PostHistoryStore::FileSummary &file : details.files) {
        const QList<PostHistoryStore::ArticleSummary> articles = details.articlesByFile.value(file.id);
        const qint64 fullArticleBytes =
            inferFullArticleBytes(file, articles, postFullArticleBytesHint);
        bool hasUnknown = false;
        bool hasNonPosted = false;
        for (const PostHistoryStore::ArticleSummary &article : articles) {
            if (article.status == QStringLiteral("unknown"))
                hasUnknown = true;
            if (article.status != QStringLiteral("posted"))
                hasNonPosted = true;
        }
        if (hasUnknown && warnings)
            *warnings << tr("file %1 contains unknown articles").arg(file.postedName);
        if (hasNonPosted && warnings)
            *warnings << tr("file %1 contains non-posted articles").arg(file.postedName);

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
            qint64 segmentBytes = article.bytes;
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
