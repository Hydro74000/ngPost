// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// NZB regeneration from structured history.
//
//========================================================================

#ifndef NZBHISTORYREGENERATOR_H
#define NZBHISTORYREGENERATOR_H

#include "history/PostHistoryStore.h"

#include <QCoreApplication>

class QTextStream;

class NzbHistoryRegenerator
{
    Q_DECLARE_TR_FUNCTIONS(NzbHistoryRegenerator)

public:
    explicit NzbHistoryRegenerator(PostHistoryStore *store);

    bool writeNzb(qint64 postId,
                  QTextStream &stream,
                  bool includePassword,
                  QStringList *warnings,
                  QString *error,
                  const QString &passwordOverride = QString());

private:
    bool _writeHeader(qint64 postId,
                      const PostHistoryStore::PostDetails &details,
                      QTextStream &stream,
                      bool includePassword,
                      QString *error,
                      const QString &passwordOverride);
    bool _writeFiles(const PostHistoryStore::PostDetails &details,
                     QTextStream &stream,
                     QStringList *warnings,
                     QString *error);
    bool _writeFile(const PostHistoryStore::PostDetails &details,
                    const PostHistoryStore::FileSummary &file,
                    QTextStream &stream,
                    int padding,
                    qint64 postFullArticleBytesHint,
                    bool hasExactArticleSize,
                    bool useBodyBytes,
                    int &repairedArticleBytes,
                    QStringList *warnings,
                    QString *error);
    void _writeSegments(const PostHistoryStore::FileSummary &file,
                        const QList<PostHistoryStore::ArticleSummary> &articles,
                        QTextStream &stream,
                        qint64 fullArticleBytes,
                        bool useBodyBytes,
                        int &repairedArticleBytes);

    PostHistoryStore *_store;
};

#endif // NZBHISTORYREGENERATOR_H
