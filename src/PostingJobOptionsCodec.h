// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// Versioned JSON envelope of the options of a queued post
// (plan_queue.md §5.3).
//
// Every field of PostingJobOptions has an explicit policy here:
//   - persisted, with its edit class (queue::FieldClass, plan §3.7);
//   - secret, never written (rarPass, declaredPassword);
//   - recomputed from the history by ResumePlanner (resume*, original*);
//   - resolved again from the configuration of the restoring instance
//     (tmpPath, rarPath, par2Path).
// A field added to PostingJobOptions must get a policy here AND in
// tst_PostingJobOptionsCodec, or it silently disappears at restoration.
//
//========================================================================

#ifndef POSTINGJOBOPTIONSCODEC_H
#define POSTINGJOBOPTIONSCODEC_H

#include "PostingJobOptions.h"
#include "queue/QueueTypes.h"

#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

//! Where the archive password of a post comes from. Decided at admission, never
//! inferred later by comparing values.
enum class PasswordOrigin {
    Absent,
    Fixed,
    Generated,
    Provided,
    Declared
};

QString passwordOriginName(PasswordOrigin origin);
bool parsePasswordOrigin(const QString &text, PasswordOrigin &origin);

//! A source as it was when the post was admitted. Size and date are coherence
//! checks, not a proof of identity.
struct PersistedSource
{
    QString path;
    bool isDir = false;
    qint64 sizeBytes = 0;
    qint64 mtimeEpoch = 0;

    bool operator==(const PersistedSource &other) const
    {
        return path == other.path && isDir == other.isDir && sizeBytes == other.sizeBytes
            && mtimeEpoch == other.mtimeEpoch;
    }
};

//! The recipe of a queued post: the job options plus what the admission decided.
struct PersistedQueueOptions
{
    PostingJobOptions job;

    PasswordOrigin passwordOrigin = PasswordOrigin::Absent;
    int passwordLength = 0; //!< length to draw again, for a generated one

    //! Archiver and PAR2 tool effectively resolved at admission ("rar" or "7zip";
    //! par2::toolName()). Freezing "Auto" would let a restoration switch tools.
    QString archiverFamily;
    QString par2Family;

    QList<PersistedSource> sources;

    //! Fields that could not be persisted safely and must be entered again
    //! before this post runs after a restoration (e.g. "rarArgs").
    QStringList omittedFields;
};

namespace optionscodec
{

constexpr int kVersion = 1;

struct EncodeResult
{
    QJsonObject json;
    QStringList omittedFields;
    QString error;

    bool ok() const { return error.isEmpty(); }
};

struct DecodeResult
{
    PersistedQueueOptions options;
    QString error;

    bool ok() const { return error.isEmpty(); }
};

//! Fills sources from job.files (one stat per file) and encodes. Fails, instead
//! of guessing, when a frozen tool family is missing.
EncodeResult encode(PersistedQueueOptions options);
DecodeResult decode(const QJsonObject &json);

//! Archiver arguments are persisted only when every token is a known option
//! without a secret of that family; SecretMasker is not a proof of that.
bool archiverArgsArePersistable(const QString &family, const QString &args);

//! Names of every persisted field, in the JSON.
QStringList persistedFields();
//! Edit class of a persisted field (or of the in-memory secrets "rarPass",
//! "declaredPassword"). Unknown names are a programming error.
queue::FieldClass fieldClass(const QString &field, bool *known = nullptr);
//! Edit classes touched between two recipes, secrets included.
QSet<queue::FieldClass> changedClasses(const PersistedQueueOptions &before,
                                       const PersistedQueueOptions &after);

} // namespace optionscodec

#endif // POSTINGJOBOPTIONSCODEC_H
