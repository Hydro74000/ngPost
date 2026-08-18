// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>

#include "postinfo/PostInfoTemplate.h"

#include "PostingJob.h" // humanSize, so that the fiche and the GUI never disagree

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>

namespace PostInfoTemplate
{

namespace
{

char const *const kDefaultDateFormat = "yyyy-MM-dd";

//! Q_DECLARE_TR_FUNCTIONS cannot be used in a namespace (it emits access
//! specifiers), so the translation context is declared by hand.
QString tr(char const *sourceText)
{
    return QCoreApplication::translate("PostInfoTemplate", sourceText);
}

//! "__nzbPath__" -> "nzbPath"
QString bareName(char const *placeholder)
{
    return QString::fromLatin1(placeholder).mid(2).chopped(2);
}

//! One pass over the template: either a __variable__ (with an optional
//! :argument stopping at the first __), or the legacy %1. Matching both in the
//! same expression is what prevents a substituted value from being expanded a
//! second time.
QRegularExpression const &tokenRegExp()
{
    static QRegularExpression const re(QStringLiteral("__([A-Za-z0-9]+)(?::((?:(?!__).)*))?__|(%1)"));
    return re;
}

QString pathValue(QString const &path, bool nativeSeparators)
{
    if (path.isEmpty() || !nativeSeparators)
        return path;
    return QDir::toNativeSeparators(path);
}

QString numberOrEmpty(int value)
{
    return value < 0 ? QString() : QString::number(value);
}

QString formatDate(QDateTime const &dt, QString const &format)
{
    if (!dt.isValid())
        return QString();
    return dt.toString(format.isEmpty() ? QString::fromLatin1(kDefaultDateFormat) : format);
}

//! True when \a path is \a other or lives below it. Used so that a source
//! folder protects every file it contains, not just its own path.
bool isSameOrBelow(QString const &path, QString const &other)
{
    QString const a = QDir::cleanPath(path);
    QString const b = QDir::cleanPath(other);
    if (a.compare(b, Qt::CaseSensitive) == 0)
        return true;
    return a.startsWith(b + QLatin1Char('/'));
}

} // namespace

QVector<FieldDoc> const &fields()
{
    // clang-format off
    static QVector<FieldDoc> const sFields = {
        // placeholder             env name                       description                                                        path  secret
        { "__originalPath__",      "NGPOST_ORIGINAL_PATH",        "folder of the posted files (legacy, unchanged)",                  true,  false },
        { "__sourcePath__",        "NGPOST_SOURCE_PATH",          "first file or folder you asked to post",                          true,  false },
        { "__originalName__",      "NGPOST_ORIGINAL_NAME",        "name of that file or folder",                                     false, false },
        { "__nzbPath__",           "NGPOST_NZB_PATH",             "full path of the nzb file",                                       true,  false },
        { "__nzbDir__",            "NGPOST_NZB_DIR",              "folder holding the nzb file",                                     true,  false },
        { "__nzbName__",           "NGPOST_NZB_NAME",             "nzb name without the .nzb extension",                             false, false },
        { "__nzbFileName__",       "NGPOST_NZB_FILE_NAME",        "nzb name with the .nzb extension",                                false, false },
        { "__rarName__",           "NGPOST_RAR_NAME",             "name of the archive, the one to search for on Usenet",            false, false },
        { "__rarPass__",           "NGPOST_RAR_PASS",             "archive password",                                                false, true  },
        { "__groups__",            "NGPOST_GROUPS",               "newsgroups the post was sent to, coma separated",                 false, false },
        { "__nzbPoster__",         "NGPOST_NZB_POSTER",           "poster declared in the nzb (not the random per article From:)",   false, false },
        { "__par2Pct__",           "NGPOST_PAR2_PCT",             "par2 redundancy percentage, empty when par2 is disabled",         false, false },
        { "__postSize__",          "NGPOST_POST_SIZE",            "bytes of rar + par2 actually posted, before yEnc encoding",       false, false },
        { "__postSizeHuman__",     "NGPOST_POST_SIZE_HUMAN",      "same size, human readable",                                       false, false },
        { "__sizeInByte__",        "NGPOST_SIZE_IN_BYTE",         "legacy size, kept unchanged for existing scripts",                false, false },
        { "__nbFiles__",           "NGPOST_NB_FILES",             "number of posted files",                                          false, false },
        { "__nbArticles__",        "NGPOST_NB_ARTICLES",          "number of articles",                                              false, false },
        { "__nbArticlesPosted__",  "NGPOST_NB_ARTICLES_POSTED",   "number of articles successfully posted",                          false, false },
        { "__nbArticlesFailed__",  "NGPOST_NB_ARTICLES_FAILED",   "number of articles that failed",                                  false, false },
        { "__avgSpeed__",          "NGPOST_AVG_SPEED",            "average upload speed",                                            false, false },
        { "__durationSec__",       "NGPOST_DURATION_SEC",         "upload duration in seconds",                                      false, false },
        { "__status__",            "NGPOST_STATUS",               "success, partial or failed",                                      false, false },
        { "__postId__",            "NGPOST_POST_ID",              "history database id, 0 when there is no history",                 false, false },
        { "__appVersion__",        "NGPOST_APP_VERSION",          "ngPost version that made the post",                               false, false },
        { "__postInfoPath__",      "NGPOST_POST_INFO_PATH",       "path of the generated post info file",                            true,  false },
        { "__jsonPath__",          "NGPOST_JSON",                 "path of the temporary json file (post commands only)",            true,  false },
    };
    // clang-format on
    return sFields;
}

QString const &passwordPlaceholder()
{
    static QString const sPass = QStringLiteral("__rarPass__");
    return sPass;
}

QMap<QString, QString> values(PostInfoData const &data, bool nativeSeparators)
{
    QMap<QString, QString> v;
    v["originalPath"]      = pathValue(data.originalPath, nativeSeparators);
    v["sourcePath"]        = pathValue(data.sourcePath, nativeSeparators);
    v["originalName"]      = data.originalName;
    v["nzbPath"]           = pathValue(data.nzbPath, nativeSeparators);
    v["nzbDir"]            = pathValue(data.nzbDir, nativeSeparators);
    v["nzbName"]           = data.nzbName;
    v["nzbFileName"]       = data.nzbFileName;
    v["rarName"]           = data.rarName;
    v["rarPass"]           = data.rarPass;
    v["groups"]            = data.groups;
    v["nzbPoster"]         = data.nzbPoster;
    v["par2Pct"]           = numberOrEmpty(data.par2Pct);
    v["postSize"]          = QString::number(data.postSizeBytes);
    v["postSizeHuman"]     = PostingJob::humanSize(static_cast<double>(data.postSizeBytes));
    v["sizeInByte"]        = QString::number(data.legacySizeBytes);
    v["nbFiles"]           = QString::number(data.nbFiles);
    v["nbArticles"]        = QString::number(data.nbArticles);
    v["nbArticlesPosted"]  = QString::number(data.nbArticlesPosted);
    v["nbArticlesFailed"]  = QString::number(data.nbArticlesFailed);
    v["avgSpeed"]          = data.avgSpeed;
    v["durationSec"]       = QString::number(data.durationSec);
    v["status"]            = data.status;
    v["postId"]            = QString::number(data.historyPostId);
    v["appVersion"]        = data.appVersion;
    v["postInfoPath"]      = pathValue(data.postInfoPath, nativeSeparators);
    v["jsonPath"]          = pathValue(data.jsonPath, nativeSeparators);
    return v;
}

QString render(QString const      &tmpl,
               PostInfoData const &data,
               bool                nativeSeparators,
               OnUnknown           onUnknown,
               QStringList        *unknown)
{
    QMap<QString, QString> const fixed = values(data, nativeSeparators);

    QString                     out;
    out.reserve(tmpl.size());
    qsizetype                   last = 0;
    QRegularExpressionMatchIterator it = tokenRegExp().globalMatch(tmpl);
    while (it.hasNext()) {
        QRegularExpressionMatch const m = it.next();
        out += tmpl.mid(last, m.capturedStart() - last);
        last = m.capturedEnd();

        if (m.capturedStart(3) >= 0) { // legacy %1
            out += pathValue(data.nzbPath, nativeSeparators);
            continue;
        }

        QString const name = m.captured(1);
        QString const arg  = m.captured(2);

        if (name == QLatin1String("date")) {
            out += formatDate(data.finishedAt, arg);
        } else if (name == QLatin1String("dateStart")) {
            out += formatDate(data.startedAt, arg);
        } else if (name == QLatin1String("meta")) {
            // an unfilled metadata is empty, never an error: templates are
            // written once and used for every post
            out += data.meta.value(arg).value;
        } else if (fixed.contains(name)) {
            out += fixed.value(name);
        } else {
            if (unknown && !unknown->contains(m.captured(0)))
                *unknown << m.captured(0);
            if (onUnknown == OnUnknown::KeepVerbatim)
                out += m.captured(0); // exactly what NZB_POST_CMD did before
        }
    }
    out += tmpl.mid(last);
    return out;
}

QStringList renderArguments(QStringList const  &args,
                            PostInfoData const &data,
                            bool                nativeSeparators,
                            QStringList        *unknown)
{
    QStringList rendered;
    rendered.reserve(args.size());
    for (QString const &arg : args)
        rendered << render(arg, data, nativeSeparators, OnUnknown::KeepVerbatim, unknown);
    return rendered;
}

QString redactSecrets(QString const &text, PostInfoData const &data)
{
    QMap<QString, QString> const v = values(data, false);

    QString redacted = text;
    for (FieldDoc const &field : fields()) {
        if (!field.isSecret)
            continue;
        QString const secret = v.value(bareName(field.placeholder));
        if (!secret.isEmpty())
            redacted.replace(secret, QStringLiteral("****"));
    }
    return redacted;
}

void applyEnvironment(QProcessEnvironment &env, PostInfoData const &data, bool exposeSecrets)
{
    QMap<QString, QString> const v = values(data, true);
    for (FieldDoc const &field : fields()) {
        if (field.isSecret && !exposeSecrets)
            continue;
        env.insert(QString::fromLatin1(field.envName), v.value(bareName(field.placeholder)));
    }
    // Metadata deliberately does not go through the environment: normalizing
    // arbitrary keys into variable names collides and mangles unicode. It is
    // passed through the json file instead.
}

QByteArray toJson(PostInfoData const &data, bool includeSecrets)
{
    QJsonObject root;
    QMap<QString, QString> const v = values(data, false);
    for (FieldDoc const &field : fields()) {
        if (field.isSecret && !includeSecrets)
            continue;
        QString const name = bareName(field.placeholder);
        root.insert(name, v.value(name));
    }
    root.insert(QStringLiteral("date"), formatDate(data.finishedAt, QString()));
    root.insert(QStringLiteral("dateStart"), formatDate(data.startedAt, QString()));
    root.insert(QStringLiteral("partial"), data.partial);

    QJsonObject meta;
    for (auto it = data.meta.cbegin(); it != data.meta.cend(); ++it) {
        QJsonObject entry;
        entry.insert(QStringLiteral("value"), it.value().value);
        entry.insert(QStringLiteral("scope"),
                     it.value().scope == MetaScope::Nzb ? QStringLiteral("nzb")
                                                        : QStringLiteral("local"));
        meta.insert(it.key(), entry);
    }
    root.insert(QStringLiteral("meta"), meta);

    QJsonArray inputs;
    for (QString const &path : data.inputPaths)
        inputs.append(path);
    root.insert(QStringLiteral("inputPaths"), inputs);

    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

QString resolveTemplatePath(QString const &path, QString const &baseDir)
{
    if (path.isEmpty())
        return path;

    QString expanded = path;
    if (expanded.startsWith(QLatin1Char('~')))
        expanded = QDir::homePath() + expanded.mid(1);

    QFileInfo fi(expanded);
    if (fi.isAbsolute() || baseDir.isEmpty())
        return QDir::cleanPath(fi.filePath());

    return QDir::cleanPath(QDir(baseDir).absoluteFilePath(expanded));
}

Result renderToFile(QString const      &templatePath,
                    QString const      &outputPattern,
                    PostInfoData const &data,
                    QStringList const  &protectedPaths)
{
    Result res;

    QFile tmplFile(templatePath);
    if (!tmplFile.open(QIODevice::ReadOnly)) {
        res.error = tr("cannot read the template %1: %2").arg(templatePath, tmplFile.errorString());
        return res;
    }
    QString const tmpl = QString::fromUtf8(tmplFile.readAll());
    tmplFile.close();

    // The destination must be exact: writing to an approximate location is worse
    // than not writing at all, so an unknown variable there is fatal.
    QStringList unknownInPath;
    QString const outPath =
        render(outputPattern, data, false, OnUnknown::Fail, &unknownInPath);
    if (!unknownInPath.isEmpty()) {
        res.error = tr("unknown variable in the output path: %1").arg(unknownInPath.join(", "));
        return res;
    }
    if (outPath.trimmed().isEmpty()) {
        res.error = tr("the output path is empty");
        return res;
    }

    QFileInfo const outFi(outPath);
    res.outPath = QDir::cleanPath(outFi.absoluteFilePath());

    for (QString const &protectedPath : protectedPaths) {
        if (protectedPath.isEmpty())
            continue;
        if (isSameOrBelow(res.outPath, QFileInfo(protectedPath).absoluteFilePath())) {
            res.error = tr("refusing to overwrite %1").arg(res.outPath);
            res.outPath.clear();
            return res;
        }
    }

    QDir const outDir = QFileInfo(res.outPath).absoluteDir();
    if (!outDir.exists() && !QDir().mkpath(outDir.absolutePath())) {
        res.error = tr("cannot create the folder %1").arg(outDir.absolutePath());
        res.outPath.clear();
        return res;
    }

    QStringList unknownInBody;
    QString const body =
        render(tmpl, data, true, OnUnknown::KeepVerbatim, &unknownInBody);
    if (!unknownInBody.isEmpty())
        res.warnings << tr("unknown variable(s) left as-is: %1").arg(unknownInBody.join(", "));

    QMap<QString, QString> const rawValues  = values(data, false);
    bool                         holdsSecret = false;
    for (FieldDoc const &field : fields()) {
        if (!field.isSecret)
            continue;
        QString const secret = rawValues.value(bareName(field.placeholder));
        if (!secret.isEmpty() && body.contains(secret)) {
            holdsSecret = true;
            break;
        }
    }

    QSaveFile file(res.outPath);
    if (!file.open(QIODevice::WriteOnly)) { // binary: the template's own line endings survive
        res.error = tr("cannot write %1: %2").arg(res.outPath, file.errorString());
        res.outPath.clear();
        return res;
    }

    if (holdsSecret) {
        // Permissions are set on the temporary file BEFORE writing: doing it
        // afterwards would let the secret sit world readable in between.
        if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
            res.warnings << tr("could not restrict the permissions of %1; it holds the archive "
                               "password, put it in a folder only you can read")
                                .arg(res.outPath);
        }
#if defined(Q_OS_WIN)
        res.warnings << tr("on Windows the post info file inherits the permissions of its folder; "
                           "it holds the archive password, so keep it in a restricted folder");
#endif
    }

    file.write(body.toUtf8()); // UTF-8, no BOM
    if (!file.commit()) {
        res.error = tr("cannot write %1: %2").arg(res.outPath, file.errorString());
        res.outPath.clear();
        return res;
    }

    res.ok = true;
    return res;
}

} // namespace PostInfoTemplate
