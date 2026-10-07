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

#include "ArticleBuilder.h"
#include "NgPost.h"
#include "Poster.h"
#include "PostingJob.h"
#include "nntp/NntpArticle.h"

ArticleBuilder::ArticleBuilder(Poster *poster, QObject *parent)
    : QObject(parent)
    , _ngPost(poster->_ngPost)
    , _poster(poster)
    , _job(poster->_job)
    , _buffer(new char[static_cast<quint64>(_job->articleSizeBytes()) + 1])
{
    connect(this,
            &ArticleBuilder::scheduleNextArticle,
            this,
            &ArticleBuilder::onPrepareNextArticle,
            Qt::QueuedConnection);
}

ArticleBuilder::~ArticleBuilder()
{
    delete[] _buffer;
}

NntpArticle *ArticleBuilder::getNextArticle(const QString &threadName)
{
    // _buffer is shared between the _builderThread and any posting thread
    // that falls back to building its own Article, so it needs a lock of its
    // own. It must stay scoped strictly inside this function: see the lock
    // ordering note in onPrepareNextArticle().
    QMutexLocker bufferLock(&_secureBuffer);

    _job->_secureDiskAccess.lock();
    NntpArticle *article = _job->_readNextArticleIntoBufferPtr(threadName, &_buffer);
    _job->_secureDiskAccess.unlock();
    if (article) {
        article->yEncBody(_buffer);
#ifdef __SAVE_ARTICLES__
        article->dumpToFile("/tmp", _ngPost->aticleSignature());
#endif
    }
    return article;
}

void ArticleBuilder::onPrepareNextArticle()
{
    // Reading the file and yEnc encoding it is milliseconds of work that
    // needs nothing from the Poster's queue. Holding _secureArticles across
    // all of it made every connection thread coming to collect an Article
    // wait out a whole encoding, on the very mutex the builder needs back to
    // hand that Article over. The lock now covers the enqueue alone.
    //
    // The order the two mutexes are taken in matters. Poster::getNextArticle()
    // holds _secureArticles and can then reach _secureBuffer through
    // getNextArticle() below, so this path must never hold _secureBuffer
    // while waiting for _secureArticles. getNextArticle() releases it before
    // returning, which is what stops the two orders from meeting.
    {
        QMutexLocker lock(&_poster->_secureArticles);
        _poster->_articleBuildInProgress = true;
    }

    NntpArticle *article = getNextArticle(_poster->_builderThread.objectName());

    {
        QMutexLocker lock(&_poster->_secureArticles); // coming from _builderThread
        if (article)
            _poster->_articles.enqueue(article);
        _poster->_articleBuildInProgress = false;
        _poster->_articleBuilt.wakeAll();
    }
}
