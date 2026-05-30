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
                  QString *error);

private:
    PostHistoryStore *_store;
};

#endif // NZBHISTORYREGENERATOR_H
