//========================================================================
//
// Copyright (C) 2020 Matthieu Bruel <Matthieu.Bruel@gmail.com>
// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3..
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>
//
//========================================================================

#ifndef POSTER_H
#define POSTER_H

#include <QMutex>
#include <QQueue>
#include <QThread>
#include <QVector>
#include <QWaitCondition>
class NgPost;
class ArticleBuilder;
class NntpConnection;
class NntpArticle;
class PostingJob;

/*!
 * \brief Poster is a container class that has 2 Threads
 *    _builderThread to prepare Articles and fill the Queue
 *    _connectionsThread to run several connections that will consume the Article queue
 *
 *  The goal is to make sure that we've the same number of Posters threads than Builders.
 *  Each time a Poster consume an Article, it schedules an event to its Builder to prepare another one
 *
 * It owns the _articleBuilder
 * but it doesn't own the NntpConnection (the PostingJob does)
 */
class Poster
{
    friend class ArticleBuilder;

    // Copying a Poster would duplicate a running thread pair and a mutex the
    // builders synchronise on. Its QThread/QMutex members already make the
    // compiler reject it; saying so here is what stops a reader from having to
    // work that out from the member list.
    Q_DISABLE_COPY(Poster)

private:
    const ushort _id;
    NgPost *const _ngPost;
    PostingJob *const _job;

    QThread _builderThread;
    QThread _connectionsThread;

    ArticleBuilder *_articleBuilder;
    QVector<NntpConnection *>
        _nntpConnections; //!< we don't own them, we just move them to _connectionsThread

    QQueue<NntpArticle *> _articles;
    QMutex _secureArticles;

    //! True while the dedicated builder has reserved an article but has not
    //! handed it to this Poster's queue yet. A temporarily empty queue is not
    //! end-of-input while this is set, even if another builder has already set
    //! PostingJob::_noMoreFiles after reserving the final source slice.
    bool _articleBuildInProgress;
    QWaitCondition _articleBuilt;

public:
    Poster(PostingJob *job, ushort id);
    ~Poster();

    void addConnection(NntpConnection *connection);

    NntpArticle *getNextArticle(const QString &conPrefix);

    inline void lockQueue();
    inline void unlockQueue();

    inline QString name() const;

    inline void startThreads();
    void stopThreads();

    void scheduleArticlesInAdvance(int rounds);

    bool isPosting() const;
    bool isPaused() const;
};

void Poster::lockQueue()
{
    _secureArticles.lock();
}
void Poster::unlockQueue()
{
    _secureArticles.unlock();
}

QString Poster::name() const
{
    return _connectionsThread.objectName();
}

void Poster::startThreads()
{
    _builderThread.start();
    _connectionsThread.start();
}

#endif // POSTER_H
