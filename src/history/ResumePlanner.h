//========================================================================
//
// Resume candidate classification.
//
//========================================================================

#ifndef RESUMEPLANNER_H
#define RESUMEPLANNER_H

#include "history/PostHistoryStore.h"

#include <QObject>

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

    explicit ResumePlanner(PostHistoryStore *store);

    Decision check(qint64 postId, QString *error = nullptr);

private:
    PostHistoryStore *_store;
};

#endif // RESUMEPLANNER_H
