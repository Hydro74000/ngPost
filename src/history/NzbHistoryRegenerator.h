//========================================================================
//
// NZB regeneration from structured history.
//
//========================================================================

#ifndef NZBHISTORYREGENERATOR_H
#define NZBHISTORYREGENERATOR_H

#include "history/PostHistoryStore.h"

class QTextStream;

class NzbHistoryRegenerator
{
public:
    explicit NzbHistoryRegenerator(PostHistoryStore *store);

    bool writeNzb(qint64 postId,
                  QTextStream &stream,
                  bool includePassword,
                  QStringList *warnings,
                  QString *error);

private:
    PostHistoryStore *_store;
};

#endif // NZBHISTORYREGENERATOR_H
