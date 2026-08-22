// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// Plain description of a finished post, used to render post info files.
//
// This struct is deliberately free of any dependency on NgPost/PostingJob so
// that it can be filled either from a live job or from the SQLite history,
// and compared between the two.
//
//========================================================================

#ifndef POSTINFODATA_H
#define POSTINFODATA_H

#include <QDateTime>
#include <QMap>
#include <QString>
#include <QStringList>

//! Where a user metadata is allowed to show up.
enum class MetaScope
{
    Local, //!< post info file only, never leaves the machine
    Nzb    //!< also published in the <head> of the NZB, which circulates
};

struct MetaValue
{
    QString   value;
    MetaScope scope = MetaScope::Local;

    MetaValue() = default;
    MetaValue(QString const &v, MetaScope s = MetaScope::Local) : value(v), scope(s) { }

    bool operator==(MetaValue const &other) const
    {
        return value == other.value && scope == other.scope;
    }
};

struct PostInfoData
{
    // paths and names
    QString originalPath; //!< legacy: directory of the last input file
    QString sourcePath;   //!< first raw input path, before CLI folder expansion
    QString originalName; //!< file name of sourcePath
    QString nzbPath;      //!< final nzb path (after any _1.nzb rename)
    QString nzbDir;
    QString nzbName;     //!< nzb base name, without the .nzb extension
    QString nzbFileName; //!< nzb name with the .nzb extension
    QString rarName;
    QString rarPass;
    QString groups;
    QString nzbPoster; //!< the poster= written in the nzb, NOT the per article From:
    QString status;    //!< success | partial | failed, same wording as the history
    QString avgSpeed;
    QString appVersion;

    // sizes and counters
    //! rar + par2, copied .nfo excluded, before yEnc. < 0 means "not recorded",
    //! which is how a post made before this existed comes back: it then renders
    //! empty rather than as a very wrong zero.
    qint64  postSizeBytes   = -1;
    quint64 legacySizeBytes = 0;  //!< __sizeInByte__, kept as-is for existing scripts
    int     par2Pct         = -1; //!< < 0 means no par2 at all, rendered as empty
    uint    nbFiles          = 0;
    uint    nbArticles       = 0;
    uint    nbArticlesPosted = 0;
    uint    nbArticlesFailed = 0;
    qint64  durationSec      = 0;
    qint64  historyPostId    = 0;

    // wall clock, always local time (the history stores UTC and converts on read)
    QDateTime startedAt;
    QDateTime finishedAt;

    //! User metadata. The password is never stored here, it is a secret handled
    //! like the archive password.
    QMap<QString, MetaValue> meta;

    //! Raw input paths, captured before folders are expanded into files. Used to
    //! refuse overwriting a source file (or anything below a source folder).
    QStringList inputPaths;

    //! True when the data had to be rebuilt from a history that predates some of
    //! these fields: the missing ones are rendered empty.
    bool partial = false;

    // contextual, only meaningful for post commands
    QString postInfoPath; //!< empty while the post info file itself is rendered
    QString jsonPath;     //!< empty outside of the post command context
};

#endif // POSTINFODATA_H
