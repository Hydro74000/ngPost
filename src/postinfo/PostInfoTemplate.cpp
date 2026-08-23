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

//! Resolves symlinks when the path exists, so that protecting a source also
//! protects the link that points at it, and vice versa.
QString resolvedPath(QString const &path)
{
    QFileInfo const fi(path);
    QString const canonical = fi.canonicalFilePath();
    if (!canonical.isEmpty())
        return canonical;
    // Nothing there (yet): the destination of a file about to be written is the
    // usual case. Resolve the parent, which normally does exist.
    QString const parent = fi.absoluteDir().canonicalPath();
    if (parent.isEmpty())
        return QDir::cleanPath(fi.absoluteFilePath());
    return QDir::cleanPath(parent + QLatin1Char('/') + fi.fileName());
}

//! True when \a path is \a other or lives below it. Used so that a source
//! folder protects every file it contains, not just its own path.
bool isSameOrBelow(QString const &path, QString const &other)
{
    // Windows and macOS do not distinguish Backup.tar from backup.TAR, and
    // treating them as different files here would defeat the protection.
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
    const Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
    QString const a = resolvedPath(path);
    QString const b = resolvedPath(other);
    if (a.compare(b, cs) == 0)
        return true;
    return a.startsWith(b + QLatin1Char('/'), cs);
}

} // namespace

QVector<FieldDoc> const &fields()
{
    // clang-format off
    static QVector<FieldDoc> const sFields = {
        // placeholder             env name                       description                                                        path  secret  known
        { "__originalPath__",      "NGPOST_ORIGINAL_PATH",        "folder of the posted files (legacy, unchanged)",                  true,  false, true  },
        { "__sourcePath__",        "NGPOST_SOURCE_PATH",          "first file or folder you asked to post",                          true,  false, true  },
        { "__originalName__",      "NGPOST_ORIGINAL_NAME",        "name of that file or folder",                                     false, false, true  },
        { "__nzbPath__",           "NGPOST_NZB_PATH",             "full path of the nzb file",                                       true,  false, true  },
        { "__nzbDir__",            "NGPOST_NZB_DIR",              "folder holding the nzb file",                                     true,  false, true  },
        { "__nzbName__",           "NGPOST_NZB_NAME",             "nzb name without the .nzb extension",                             false, false, true  },
        { "__nzbFileName__",       "NGPOST_NZB_FILE_NAME",        "nzb name with the .nzb extension",                                false, false, true  },
        { "__rarName__",           "NGPOST_RAR_NAME",             "name of the archive, the one to search for on Usenet",            false, false, true  },
        { "__rarPass__",           "NGPOST_RAR_PASS",             "archive password",                                                false, true, true   },
        { "__groups__",            "NGPOST_GROUPS",               "newsgroups the post was sent to, coma separated",                 false, false, true  },
        { "__nzbPoster__",         "NGPOST_NZB_POSTER",           "poster declared in the nzb (not the random per article From:)",   false, false, true  },
        { "__par2Pct__",           "NGPOST_PAR2_PCT",             "par2 redundancy percentage, empty when par2 is disabled",         false, false, true  },
        { "__postSize__",          "NGPOST_POST_SIZE",            "bytes of rar + par2 actually posted, before yEnc encoding",       false, false, false },
        { "__postSizeHuman__",     "NGPOST_POST_SIZE_HUMAN",      "same size, human readable",                                       false, false, false },
        { "__sizeInByte__",        "NGPOST_SIZE_IN_BYTE",         "legacy size, kept unchanged for existing scripts",                false, false, false },
        { "__nbFiles__",           "NGPOST_NB_FILES",             "number of posted files",                                          false, false, false },
        { "__nbArticles__",        "NGPOST_NB_ARTICLES",          "number of articles",                                              false, false, false },
        { "__nbArticlesPosted__",  "NGPOST_NB_ARTICLES_POSTED",   "number of articles successfully posted",                          false, false, false },
        { "__nbArticlesFailed__",  "NGPOST_NB_ARTICLES_FAILED",   "number of articles that failed",                                  false, false, false },
        { "__avgSpeed__",          "NGPOST_AVG_SPEED",            "average upload speed",                                            false, false, false },
        { "__durationSec__",       "NGPOST_DURATION_SEC",         "upload duration in seconds",                                      false, false, false },
        { "__status__",            "NGPOST_STATUS",               "success, partial or failed",                                      false, false, false },
        { "__postId__",            "NGPOST_POST_ID",              "history database id, 0 when there is no history",                 false, false, false },
        { "__appVersion__",        "NGPOST_APP_VERSION",          "ngPost version that made the post",                               false, false, true  },
        { "__postInfoPath__",      "NGPOST_POST_INFO_PATH",       "path of the generated post info file",                            true,  false, false },
        { "__jsonPath__",          "NGPOST_JSON",                 "path of the temporary json file (post commands only)",            true,  false, false },
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
    v["postSize"]      = data.postSizeBytes < 0 ? QString() : QString::number(data.postSizeBytes);
    v["postSizeHuman"] = data.postSizeBytes < 0
                             ? QString()
                             : PostingJob::humanSize(static_cast<double>(data.postSizeBytes));
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

Escape escapeModeIn(QString const &tmpl, QString *unknownFormat)
{
    // Only on a comment line, and only at the very start of it: the directive
    // is then dropped with the other comments and never reaches the sheet.
    for (SheetLine const &line : parseTemplate(tmpl)) {
        if (line.kind != SheetLine::Kind::Comment)
            continue;
        if (!line.raw.startsWith(QLatin1String("#!")))
            continue;

        QString const format = line.raw.mid(2).trimmed().toLower();
        if (format == QLatin1String("json"))
            return Escape::Json;
        if (format == QLatin1String("xml"))
            return Escape::Xml;
        if (unknownFormat && unknownFormat->isEmpty())
            *unknownFormat = format;
    }
    return Escape::None;
}

QString escapeValue(QString const &value, Escape mode)
{
    switch (mode) {
    case Escape::None:
        return value;

    case Escape::Xml: {
        QString out = value;
        out.replace(QLatin1Char('&'), QLatin1String("&amp;"));
        out.replace(QLatin1Char('<'), QLatin1String("&lt;"));
        out.replace(QLatin1Char('>'), QLatin1String("&gt;"));
        out.replace(QLatin1Char('"'), QLatin1String("&quot;"));
        out.replace(QLatin1Char('\''), QLatin1String("&apos;"));
        return out;
    }

    case Escape::Json: {
        // RFC 8259: the two mandatory ones, the shorthands, and everything
        // below 0x20 as \uXXXX. A raw newline in a title is what breaks a
        // hand written JSON model first.
        QString out;
        out.reserve(value.size());
        for (QChar const c : value) {
            switch (c.unicode()) {
            case '"':  out += QLatin1String("\\\""); break;
            case '\\': out += QLatin1String("\\\\"); break;
            case '\b': out += QLatin1String("\\b"); break;
            case '\f': out += QLatin1String("\\f"); break;
            case '\n': out += QLatin1String("\\n"); break;
            case '\r': out += QLatin1String("\\r"); break;
            case '\t': out += QLatin1String("\\t"); break;
            default:
                if (c.unicode() < 0x20)
                    out += QStringLiteral("\\u%1").arg(
                        static_cast<uint>(c.unicode()), 4, 16, QLatin1Char('0'));
                else
                    out += c; // UTF-8 goes through as itself, json allows it
                break;
            }
        }
        return out;
    }
    }
    return value;
}

QString render(QString const      &tmpl,
               PostInfoData const &data,
               bool                nativeSeparators,
               OnUnknown           onUnknown,
               QStringList        *unknown,
               bool                legacyPercentOne,
               Escape              escape)
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

        if (m.capturedStart(3) >= 0) { // legacy %1 of NZB_POST_CMD
            out += legacyPercentOne ? pathValue(data.nzbPath, nativeSeparators) : m.captured(0);
            continue;
        }

        QString const name = m.captured(1);
        QString const arg  = m.captured(2);

        if (name == QLatin1String("date")) {
            out += escapeValue(formatDate(data.finishedAt, arg), escape);
        } else if (name == QLatin1String("dateStart")) {
            out += escapeValue(formatDate(data.startedAt, arg), escape);
        } else if (name == QLatin1String("meta")) {
            // an unfilled metadata is empty, never an error: templates are
            // written once and used for every post
            out += escapeValue(data.meta.value(arg).value, escape);
        } else if (fixed.contains(name)) {
            out += escapeValue(fixed.value(name), escape);
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
        rendered << render(arg,
                           data,
                           nativeSeparators,
                           OnUnknown::KeepVerbatim,
                           unknown,
                           /*legacyPercentOne*/ true);
    return rendered;
}

QStringList metaNamesIn(QString const &tmpl)
{
    QStringList                     names;
    QString const                   body = stripComments(tmpl);
    QRegularExpressionMatchIterator it   = tokenRegExp().globalMatch(body);
    while (it.hasNext()) {
        QRegularExpressionMatch const m = it.next();
        if (m.captured(1) != QLatin1String("meta"))
            continue;
        QString const name = m.captured(2);
        if (!name.isEmpty() && !names.contains(name))
            names << name;
    }
    return names;
}

bool usesCrLf(QString const &tmpl) { return tmpl.contains(QLatin1String("\r\n")); }

QVector<SheetLine> parseTemplate(QString const &tmpl)
{
    QVector<SheetLine> lines;
    // Split on '\n' and drop the '\r' of a CRLF file: keeping it would leave a
    // stray carriage return at the end of every expression, and buildTemplate()
    // puts the right ending back.
    QString normalized = tmpl;
    normalized.replace(QLatin1String("\r\n"), QLatin1String("\n"));

    for (QString const &line : normalized.split(QLatin1Char('\n'))) {
        SheetLine parsed;

        // Column 0 only, deliberately: a line indented by one space is written
        // out, which is the escape hatch for a sheet that really starts with a
        // '#'. No backslash escaping, no second rule to remember.
        if (line.startsWith(QLatin1Char('#'))) {
            parsed.kind = SheetLine::Kind::Comment;
            parsed.raw  = line;
            lines << parsed;
            continue;
        }

        qsizetype const eq = line.indexOf(QLatin1Char('='));
        if (eq < 0) {
            parsed.kind = SheetLine::Kind::Raw;
            parsed.raw  = line;
            lines << parsed;
            continue;
        }

        // Split on the FIRST '=': a value can hold others, an url usually does.
        // The spaces around it are kept apart so that an aligned model stays
        // aligned when it is saved again.
        qsizetype labelEnd = eq;
        while (labelEnd > 0 && line.at(labelEnd - 1).isSpace())
            --labelEnd;
        qsizetype valueStart = eq + 1;
        while (valueStart < line.size() && line.at(valueStart).isSpace())
            ++valueStart;

        parsed.kind       = SheetLine::Kind::Field;
        parsed.label      = line.left(labelEnd);
        parsed.separator  = line.mid(labelEnd, valueStart - labelEnd);
        parsed.expression = line.mid(valueStart);
        lines << parsed;
    }

    // A file ending with a newline splits into a last empty piece; it is a real
    // line of the model as far as the round trip is concerned.
    return lines;
}

QString buildTemplate(QVector<SheetLine> const &lines, bool crlf)
{
    QStringList texts;
    texts.reserve(lines.size());
    for (SheetLine const &line : lines)
        texts << line.text();
    return texts.join(crlf ? QLatin1String("\r\n") : QLatin1String("\n"));
}

QString stripComments(QString const &tmpl)
{
    QVector<SheetLine> const lines = parseTemplate(tmpl);
    QVector<SheetLine>       kept;
    kept.reserve(lines.size());
    for (SheetLine const &line : lines) {
        if (line.kind != SheetLine::Kind::Comment)
            kept << line;
    }
    return buildTemplate(kept, usesCrLf(tmpl));
}

QVector<Token> tokensIn(QString const &tmpl)
{
    QVector<Token>                  tokens;
    QString const                   body = stripComments(tmpl);
    QRegularExpressionMatchIterator it   = tokenRegExp().globalMatch(body);
    while (it.hasNext()) {
        QRegularExpressionMatch const m = it.next();
        if (m.captured(1).isEmpty())
            continue; // the legacy "%1", which names no variable

        Token token;
        token.name = m.captured(1);
        token.arg  = m.captured(2);
        token.raw  = m.captured(0);

        // Same variable, same argument, listed once. "__date:dd/MM__" and
        // "__date:yyyy__" are two different lines of the sheet though.
        bool known = false;
        for (Token const &seen : tokens) {
            if (seen.name == token.name && seen.arg == token.arg) {
                known = true;
                break;
            }
        }
        if (!known)
            tokens << token;
    }
    return tokens;
}

QString describe(Token const &token)
{
    if (token.isMeta())
        return QCoreApplication::translate("PostInfoTemplate",
                                           "your own field, you fill it in below");

    if (token.name == QLatin1String("date"))
        return QCoreApplication::translate("PostInfoTemplate",
                                           "date the post finished, in the format you give");
    if (token.name == QLatin1String("dateStart"))
        return QCoreApplication::translate("PostInfoTemplate",
                                           "date the post started, in the format you give");

    QString const placeholder = QStringLiteral("__%1__").arg(token.name);
    for (FieldDoc const &field : fields()) {
        if (placeholder == QLatin1String(field.placeholder))
            return QCoreApplication::translate("PostInfoTemplate", field.description);
    }
    return QString();
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
        QString const name = QString::fromLatin1(field.envName);
        if (field.isSecret && !exposeSecrets) {
            // Removed, not skipped: the environment we start from is the one of
            // ngPost itself, and it may already carry an NGPOST_RAR_PASS that
            // has nothing to do with this post.
            env.remove(name);
            continue;
        }
        env.insert(name, v.value(bareName(field.placeholder)));
    }
    // The parameterized variables have no fixed placeholder, but a script
    // still needs the date: give it in ISO, plus the epoch for arithmetic.
    if (data.finishedAt.isValid()) {
        env.insert(QStringLiteral("NGPOST_DATE"), data.finishedAt.toString(Qt::ISODate));
        env.insert(QStringLiteral("NGPOST_DATE_EPOCH"),
                   QString::number(data.finishedAt.toSecsSinceEpoch()));
    }
    if (data.startedAt.isValid())
        env.insert(QStringLiteral("NGPOST_DATE_START"), data.startedAt.toString(Qt::ISODate));

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
                    QStringList const  &protectedPaths,
                    QString const      &outputBaseDir)
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

    // A relative destination follows the same rule as the model: it is
    // understood from the configuration, not from wherever the process happens
    // to have been started, which for the GUI or a service means nothing.
    QFileInfo outFi(outPath);
    if (outFi.isRelative() && !outputBaseDir.isEmpty())
        outFi = QFileInfo(QDir(outputBaseDir).absoluteFilePath(outPath));
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

    // The model says what it is; the values are escaped for that format and
    // nothing else is touched. A plain text model escapes nothing, which is
    // what lets any separator, any wording and any prose work.
    QString       unknownFormat;
    Escape const  escape = escapeModeIn(tmpl, &unknownFormat);
    if (!unknownFormat.isEmpty())
        res.warnings << tr("unknown model format '%1', values are written as they are "
                           "(known formats: json, xml)")
                            .arg(unknownFormat);

    QStringList unknownInBody;
    QString const body = render(
        stripComments(tmpl), data, true, OnUnknown::KeepVerbatim, &unknownInBody, false, escape);
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
