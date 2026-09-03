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

#ifndef POSTINGJOB_H
#define POSTINGJOB_H
#include "PostingJobOptions.h"
#include "utils/Macros.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFileInfoList>
#include <QMap>
#include <QMutex>
#include <QQueue>
#include <QSet>
#include <QStringList>
#include <QTextStream>
#include <QThread>
#include <QTime>
#include <QTimer>
#include <QVector>
class QProcess;
class NgPost;
class NntpConnection;
class NntpFile;
class NntpArticle;
class PostingWidget;
class Poster;

using AtomicBool = QAtomicInteger<unsigned short>; // 16 bit only (faster than using 8 bit variable...)

/*!
 * \brief PostingJob is an active object that will do a posting job
 * it will be possible to move it to a thread to not freeze the HMI
 * it will signal it's progressbarion to PostingWidget
 *
 * NgPost will only have one PostingJob active at a time
 * but it will be able to hold a Pending queue
 */
class PostingJob : public QObject
{
    Q_OBJECT
    friend class Poster;
    friend class ArticleBuilder;
    friend class NgPost;

public:
    //! Kept as a nested name for the existing call sites; the definition lives
    //! in PostingJobOptions.h so that the options can carry it.
    using ResumeFileState = PostingJobResumeFileState;

private:
    NgPost *const _ngPost; //!< handle on the application to access global configs

    //! Frozen at construction: a job may wait in the queue while the global
    //! settings change, and it must be posted with the settings it was queued
    //! with. Also carries the facts (raw input paths, metadata, what the
    //! original post did) that describe the job without driving it.
    const PostingJobOptions _options;

    //! Payload boundary frozen for this job. It is deliberately independent
    //! from NgPost::articleSize(), which may change while the job is queued.
    const qint64 _articleSizeBytes;

    QFileInfoList _files;  //!< populated on constuction using a QStringList of paths

    PostingWidget *const _postWidget;

    QProcess *_extProc;
    QDir *_compressDir;
    bool _limitProcDisplay;
    bool _extProcIsPar2;
    ushort _nbProcDisp;

#ifdef __USE_TMP_RAM__
    QString _tmpPath; //!< can be overwritten by _ngPost->_ramPath
#else
    const QString _tmpPath;
#endif
    const QString _rarPath;
    QString _rarArgs;
    const uint _rarSize;
    const bool _useRarMax;
    const uint _par2Pct;

    const bool _doCompress;
    const bool _doPar2;
    const QString _rarName;
    const QString _rarPass;
    const bool _keepRar;
    bool _splitArchive;

    QVector<NntpConnection *> _nntpConnections;   //!< the NNTP connections (owning the TCP sockets)
    QVector<NntpConnection *> _closedConnections; //!< the NNTP connections (owning the TCP sockets)

    QString _nzbName;                  //!< name of nzb that we'll write (without the extension)
    QQueue<NntpFile *> _filesToUpload; //!< list of files to upload (that we didn't start)
    QSet<NntpFile *> _filesInProgress; //!< files in queue to be uploaded (Articles have been produced)
    QSet<NntpFile *> _filesFailed;     //!< files that couldn't be read
    uint _nbFiles;                     //!< number of files to post in this iteration
    uint _nbPosted;                    //!< number of files posted

    QString _originalDirectory;

    QString _nzbFilePath;
    QFile *_nzb;            //!< nzb file that will be filled on the fly when a file is fully posted
    QTextStream _nzbStream; //!< txt stream for the nzb file

    NntpFile *_nntpFile; //!< current file that is getting processed
    QFile *_file;        //!< file handler on the file getting processed
    uint _part;          //!< part number (Article) on the current file

    QElapsedTimer _timeStart;  //!< to get some stats (upload time and avg speed)
    quint64 _totalSize;        //!< bytes of the files that went through, legacy value
    //! Bytes of the archive and its parity, copied .nfo excluded, before yEnc
    //! encoding. Unlike _totalSize this is known upfront and does not shrink
    //! when a file fails, so it describes what the post IS.
    quint64 _postSizeBytes;
    //! Absolute paths of the .nfo copied next to the rar volumes (KEEP_NFO_EXTENSION):
    //! posted, but not part of the archive.
    QSet<QString> _copiedNfoPaths;
    QDateTime _startedAtWall;  //!< wall clock, _timeStart is monotonic only
    QDateTime _finishedAtWall;
    QElapsedTimer _pauseTimer; //!< to record the time when ngPost is in pause
    qint64 _pauseDuration;     //!< total duration of all pauses

    int _nbConnections; //!< available number of NntpConnection (we may loose some)
    int _nbThreads;     //!< size of the ThreadPool

    uint _nbArticlesUploaded; //!< number of Articles that have been uploaded (+ failed ones)
    uint _nbArticlesFailed;   //!< number of Articles that failed to be uploaded
    quint64 _uploadedSize;    //!< bytes posted (to compute the avg speed)
    uint _nbArticlesTotal;    //!< number of Articles of all the files to post

    AtomicBool _stopPosting;
    AtomicBool _noMoreFiles;

    bool _postStarted;
    bool _packed;
    bool _postFinished;

    const bool _obfuscateArticles;
    const bool _obfuscateFileName;

    AtomicBool _delFilesAfterPost;
    const QFileInfoList _originalFiles;

    QString _nfoSrcToCopy; //!< absolute path of the .nfo to copy next to the nzb (resolved at job start, copied on success)
    QString _postInfoFilePath; //!< post info file once written, empty otherwise
    PostInfoData _finalPostInfoData; //!< consolidated description, cached
    bool _finalPostInfoDataReady = false;
    //! The streamed NZB remains authoritative when the history could not be
    //! finalized; regenerating/exporting from stale rows would corrupt facts.
    bool _historyDataUnreliable = false;

    QMutex _secureDiskAccess;

    QVector<Poster *> _posters;

    const bool _overwriteNzb;

    QMap<QString, QString> _obfuscatedFileNames; //!< obfuscated path -> real path, undone after compression
    QString _obfuscationStagingPath;              //!< folder the obfuscated inputs were moved to (empty if none)

    const QList<QString>
        _grpList; //!< Newsgroup where we're posting in a list format to write in the nzb file
    const std::string _from; //!< email of poster (if empty, random one will be used for each file)

    bool _use7z;

    bool _isPaused;

    QTimer _resumeTimer;

    bool _isActiveJob;
    qint64 _historyPostId;
    const bool _resumeFromHistory;
    const QMap<QString, ResumeFileState> _resumeFileStatesByPath;

#ifdef __COMPUTE_IMMEDIATE_SPEED__
    quint64 _immediateSize; //!< bytes posted (to compute the avg speed)
    QTimer _immediateSpeedTimer;
    QString _immediateSpeed;
    const bool _useHMI;
#endif

public:
    PostingJob(NgPost *ngPost,
               const PostingJobOptions &options,
               PostingWidget *postWidget = nullptr,
               QObject *parent = nullptr);
    ~PostingJob();

    qint64 articleSizeBytes() const { return _articleSizeBytes; }

    void pause();
    void resume();

    inline QString avgSpeed() const;

    inline void articlePosted(quint64 size);
    inline void articleFailed(quint64 size);

    inline uint nbArticlesTotal() const;
    inline uint nbArticlesUploaded() const;
    inline uint nbArticlesFailed() const;
    inline bool hasUploaded() const;

    inline const QString &nzbName() const;
    inline const QString &rarName() const;
    inline const QString &rarPass() const;
    inline QString postSize() const;

    //! Bytes of the archive and its parity, copied .nfo excluded, before yEnc.
    //! Known as soon as the upload list is built, so it describes the post even
    //! when some files end up failing.
    inline quint64 postSizeInBytes() const;
    inline QString postSizeHuman() const;
    //! par2 redundancy to DESCRIBE the post: the original one for a resume,
    //! and < 0 when there is no par2 at all.
    inline int describedPar2Pct() const;
    //! Poster written in the nzb. Under article obfuscation the real From: of
    //! each article is random and differs from this one.
    inline QString nzbPoster() const;
    //! First path the user actually gave, before a folder was expanded.
    inline QString sourcePath() const;
    inline QString sourceName() const;
    inline const QStringList &inputPaths() const;
    inline const QMap<QString, MetaValue> &postMeta() const;
    inline qint64 historyPostId() const;
    inline bool isResumeFromHistory() const;

    //! Description of this post, for a post info file or a post command. Once
    //! _buildFinalPostInfoData() has run, this is the consolidated view: for a
    //! resume the live job only knows the leftovers, so both the sheet and the
    //! hooks must read the same finalised description.
    PostInfoData postInfoData() const;
    inline const QDateTime &startedAtWall() const;
    inline const QDateTime &finishedAtWall() const;

    inline bool hasCompressed() const;
    inline bool hasPacking() const;
    inline bool isPacked() const;
    inline bool hasPostStarted() const;
    inline bool hasPostFinished() const;
    inline bool hasPostFinishedSuccessfully() const;

    //! The success rule itself, so it can be exercised without a live post.
    inline static bool postSucceeded(bool postFinished, uint nbArticlesFailed, bool anyFileFailed);

    inline PostingWidget *widget() const;

    inline QString getFirstOriginalFile() const;

    inline void setDelFilesAfterPosted(bool delFiles);

    inline QString groups() const;
    inline QString from() const;

    inline bool isPosting() const;

    inline bool isPaused() const;

    inline const QString &nzbFilePath() const;
    inline const QString &originalDirectory() const;

    inline static QString humanSize(double size);

#ifdef __COMPUTE_IMMEDIATE_SPEED__
    inline const QString &immediateSpeed() const;
#endif

    static QString sslSupportInfo();
    static bool supportsSsl();

    qint64 registerHistoryFile(int ordinal, const QFileInfo &file, NntpFile *nntpFile);
    void recordHistoryArticlePosting(NntpArticle *article, int attemptNo);
    void recordHistoryArticlePosted(NntpArticle *article);
    void recordHistoryArticleFailed(NntpArticle *article, const QString &reason);
    void recordHistoryArticleUnknown(NntpArticle *article, const QString &reason);

#ifdef NGPOST_TESTING
    static QStringList buildPar2ArgsForTest(const QString &configuredArgs,
                                            bool useParPar,
                                            bool useMultiPar,
                                            uint redundancy);
    static bool restoreObfuscatedPathsForTest(QMap<QString, QString> &paths,
                                              QString &stagingPath);
#endif

signals:
    void startPosting(
        bool isActiveJob); //!< connected to onStartPosting (to be able to run on a different Thread)
    void stopPosting();

    void postingStarted(); //!< emitted at the end of onStartPosting
    void noMoreConnection();
    void postingFinished();

    void archiveFileNames(QStringList paths);
    void articlesNumber(uint nbArticles);

    void filePosted(QString filePath, uint nbArticles, uint nbFailed);

    void packingDone();

public slots:
    void onStopPosting(); //!< for HMI

private slots:
    void onStartPosting(bool isActiveJob);
    void onDisconnectedConnection(NntpConnection *con);

    void onNntpFileStartPosting();
    void onNntpFilePosted();
    void onNntpErrorReading();

    void onExtProcReadyReadStandardOutput();
    void onExtProcReadyReadStandardError();

    void onCompressionFinished(int exitCode);
    void onGenPar2Finished(int exitCode);

    void onResumeTriggered();

#ifdef __COMPUTE_IMMEDIATE_SPEED__
    void onImmediateSpeedComputation();
#endif

private:
    void _log(const QString &aMsg, bool newline = true) const; //!< log function for QString
    void _error(const QString &error) const;
    //! What this job alone knows, before the history consolidates it.
    PostInfoData _livePostInfoData() const;
    //! Merges the history into the live view and caches the result, so the
    //! sheet and the post commands describe the same thing. Returns false when
    //! the post cannot be described at all (a resume whose history is
    //! unreadable).
    bool _buildFinalPostInfoData();
    //! Writes the post info file if one was asked for. Never fails a post:
    //! every problem is reported as a warning.
    void _writePostInfoFile();
    QStringList _protectedPaths() const;

    //! Visible in the log, but does NOT mark the run as failed. _error() sets
    //! COMPLETED_WITH_ERRORS, which would be wrong for something that did not
    //! harm the post itself, such as a post info file that could not be
    //! written.
    void _warn(const QString &warning) const;

    int _createNntpConnections();
    void _preparePostersArticles();

    NntpArticle *_readNextArticleIntoBufferPtr(const QString &threadName, char **bufferPtr);

    void _delOriginalFiles();

    void _obfuscateInputFileNames(QString const &tmpFolder, QString const &archiveName);
    bool _restoreObfuscatedFileNames();

    void _resolveNfoSource();
    void _copyNfoNextToNzb();

    inline NntpFile *_getNextFile();

    //! Builds the upload queue. Returns false when the job must not start at
    //! all, which a resume does when one of its sources no longer matches.
    bool _initPosting();
    //! Ends a job that failed before any article could be transferred. Normal
    //! jobs are finalized as failed in history; a refused resume keeps the
    //! original post resumable.
    void _abortBeforeTransfer(bool keepResumeResumable = false);
    void _postFiles();
    void _finishPosting();

    void _flushHistoryService();

    void _closeNzb();
    void _printStats() const;

    bool startCompressFiles(const QString &cmdRar,
                            const QString &tmpFolder,
                            const QString &archiveName,
                            const QString &pass,
                            uint volSize = 0);
    bool startGenPar2(const QString &tmpFolder, const QString &archiveName, uint redundancy = 0);

    bool _canCompress() const;
    bool _canGenPar2() const;

    void _cleanExtProc();
    void _cleanCompressDir();

    QString _createArchiveFolder(const QString &tmpFolder, const QString &archiveName);

    bool _checkTmpFolder() const;

    qint64 _dirSize(const QString &path);

    inline QString timestamp() const;
};

QString PostingJob::avgSpeed() const
{
    QString power = " ";
    double bandwidth = 0.;

    if (_timeStart.isValid()) {
        double sec = (_timeStart.elapsed() - _pauseDuration) / 1000.;
        bandwidth = _uploadedSize / sec;

        if (bandwidth > 1024) {
            bandwidth /= 1024;
            power = "k";
        }
        if (bandwidth > 1024) {
            bandwidth /= 1024;
            power = "M";
        }
    }

    return QString("%1 %2B/s").arg(bandwidth, 6, 'f', 2).arg(power);
}

NntpFile *PostingJob::_getNextFile()
{
    if (_filesToUpload.size()) {
        NntpFile *file = _filesToUpload.dequeue();
        _filesInProgress.insert(file);
        return file;
    } else
        return nullptr;
}

QString PostingJob::timestamp() const
{
    return QTime::currentTime().toString("hh:mm:ss.zzz");
}

void PostingJob::articlePosted(quint64 size)
{
    _uploadedSize += size;
    ++_nbArticlesUploaded;
#ifdef __COMPUTE_IMMEDIATE_SPEED__
    if (_useHMI)
        _immediateSize += size;
#endif
}

void PostingJob::articleFailed(quint64 size)
{
    _uploadedSize += size;
    ++_nbArticlesUploaded;
    ++_nbArticlesFailed;
#ifdef __COMPUTE_IMMEDIATE_SPEED__
    if (_useHMI)
        _immediateSize += size;
#endif
}

uint PostingJob::nbArticlesTotal() const
{
    return _nbArticlesTotal;
}
uint PostingJob::nbArticlesUploaded() const
{
    return _nbArticlesUploaded;
}
uint PostingJob::nbArticlesFailed() const
{
    return _nbArticlesFailed;
}
bool PostingJob::hasUploaded() const
{
    return _nbArticlesTotal > 0;
}

const QString &PostingJob::nzbName() const
{
    return _nzbName;
}
const QString &PostingJob::rarName() const
{
    return _rarName;
}
const QString &PostingJob::rarPass() const
{
    return _rarPass;
}
QString PostingJob::postSize() const
{
    return humanSize(static_cast<double>(_totalSize));
}

quint64 PostingJob::postSizeInBytes() const
{
    return _postSizeBytes;
}
QString PostingJob::postSizeHuman() const
{
    return humanSize(static_cast<double>(_postSizeBytes));
}
int PostingJob::describedPar2Pct() const
{
    return _options.describedPar2Pct();
}
QString PostingJob::nzbPoster() const
{
    return QString::fromStdString(_from);
}
QString PostingJob::sourcePath() const
{
    return _options.inputPaths.isEmpty() ? QString() : _options.inputPaths.first();
}
QString PostingJob::sourceName() const
{
    QString const path = sourcePath();
    return path.isEmpty() ? QString() : QFileInfo(path).fileName();
}
const QStringList &PostingJob::inputPaths() const
{
    return _options.inputPaths;
}
const QMap<QString, MetaValue> &PostingJob::postMeta() const
{
    return _options.meta;
}
qint64 PostingJob::historyPostId() const
{
    return _historyPostId;
}
bool PostingJob::isResumeFromHistory() const
{
    return _resumeFromHistory;
}
const QDateTime &PostingJob::startedAtWall() const
{
    return _startedAtWall;
}
const QDateTime &PostingJob::finishedAtWall() const
{
    return _finishedAtWall;
}

QString PostingJob::humanSize(double size)
{
    QString unit = "B";
    if (size > 1024) {
        size /= 1024;
        unit = "kB";
    }
    if (size > 1024) {
        size /= 1024;
        unit = "MB";
    }
    if (size > 1024) {
        size /= 1024;
        unit = "GB";
    }
    return QString("%1 %2").arg(size, 0, 'f', 2).arg(unit);
}

bool PostingJob::hasCompressed() const
{
    return _doCompress;
}
inline bool PostingJob::hasPacking() const
{
    return _doCompress || _doPar2;
}
bool PostingJob::isPacked() const
{
    return _packed;
}
bool PostingJob::hasPostStarted() const
{
    return _postStarted;
}
bool PostingJob::hasPostFinished() const
{
    return _postFinished;
}
bool PostingJob::hasPostFinishedSuccessfully() const
{
    return postSucceeded(_postFinished, _nbArticlesFailed, !_filesFailed.isEmpty());
}
bool PostingJob::postSucceeded(bool postFinished, uint nbArticlesFailed, bool anyFileFailed)
{
    // A file that could not be read never produces a failed ARTICLE: it is put
    // aside in _filesFailed and its articles are simply never built. Looking at
    // nbArticlesFailed alone therefore called a post with a whole missing file
    // a success.
    return postFinished && !nbArticlesFailed && !anyFileFailed;
}

PostingWidget *PostingJob::widget() const
{
    return _postWidget;
}

QString PostingJob::getFirstOriginalFile() const
{
    if (_originalFiles.isEmpty())
        return QString();
    else
        return _originalFiles.first().absoluteFilePath();
}

void PostingJob::setDelFilesAfterPosted(bool delFiles)
{
    _delFilesAfterPost = delFiles ? 0x1 : 0x0;
}

QString PostingJob::groups() const
{
    return _grpList.join(",");
}
QString PostingJob::from() const
{
    return _obfuscateArticles ? QString() : QString::fromStdString(_from);
}

bool PostingJob::isPosting() const
{
    return MB_LoadAtomic(_stopPosting) == 0x0;
}
bool PostingJob::isPaused() const
{
    return _isPaused;
}

const QString &PostingJob::nzbFilePath() const
{
    return _nzbFilePath;
}

const QString& PostingJob::originalDirectory() const
{
    return _originalDirectory;
}

#ifdef __COMPUTE_IMMEDIATE_SPEED__
const QString &PostingJob::immediateSpeed() const
{
    return _immediateSpeed;
}
#endif

#endif // POSTINGJOB_H
