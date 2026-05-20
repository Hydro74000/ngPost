//========================================================================
//
// NZB regeneration from structured history.
//
//========================================================================

#include "history/NzbHistoryRegenerator.h"

#include "NgPost.h"

#include <QDateTime>
#include <QTextStream>

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
            *error = QStringLiteral("history store is not available");
        return false;
    }

    PostHistoryStore::PostDetails details;
    if (!_store->loadPostDetails(postId, &details, error))
        return false;

    if (details.post.status == QStringLiteral("partial") && warnings)
        *warnings << QStringLiteral("post is partial; regenerated NZB may be incomplete");
    if (details.post.hasPassword && !details.post.passwordStored && warnings)
        *warnings << QStringLiteral("post had an archive password, but it is not stored");

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

    for (const PostHistoryStore::FileSummary &file : details.files) {
        const QList<PostHistoryStore::ArticleSummary> articles = details.articlesByFile.value(file.id);
        bool hasUnknown = false;
        bool hasNonPosted = false;
        for (const PostHistoryStore::ArticleSummary &article : articles) {
            if (article.status == QStringLiteral("unknown"))
                hasUnknown = true;
            if (article.status != QStringLiteral("posted"))
                hasNonPosted = true;
        }
        if (hasUnknown && warnings)
            *warnings << QStringLiteral("file %1 contains unknown articles").arg(file.postedName);
        if (hasNonPosted && warnings)
            *warnings << QStringLiteral("file %1 contains non-posted articles").arg(file.postedName);

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
            stream << tab << tab << tab << "<segment"
                   << " bytes=\"" << article.bytes << "\""
                   << " number=\"" << article.part << "\">"
                   << escapeXml(article.msgId)
                   << "</segment>\n";
        }
        stream << tab << tab << "</segments>\n"
               << tab << "</file>\n";
    }

    stream << "</nzb>\n";
    return true;
}
