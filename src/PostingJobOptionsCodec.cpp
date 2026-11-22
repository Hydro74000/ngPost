// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// See PostingJobOptionsCodec.h. One table describes every persisted field:
// encoding, decoding, the list of persisted fields and the edit classes all
// read it, so they cannot drift apart.
//
//========================================================================

#include "PostingJobOptionsCodec.h"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>
#include <QRegularExpression>

#include <functional>
#include <limits>
#include <vector>

using queue::FieldClass;

namespace
{

struct Field
{
    const char *name = nullptr;
    FieldClass cls = FieldClass::ArtifactRecipe;
    bool required = false;
    std::function<QJsonValue(const PersistedQueueOptions &)> get;
    std::function<bool(PersistedQueueOptions &, const QJsonValue &)> set; //!< false: bad type
};

bool integralIn(const QJsonValue &v, double min, double max)
{
    if (!v.isDouble())
        return false;
    const double d = v.toDouble();
    return d >= min && d <= max && d == static_cast<double>(static_cast<qint64>(d));
}

QJsonArray toArray(const QStringList &list)
{
    QJsonArray array;
    for (const QString &s : list)
        array.append(s);
    return array;
}

bool toStringList(const QJsonValue &v, QStringList &out)
{
    if (!v.isArray())
        return false;
    out.clear();
    for (const QJsonValue item : v.toArray()) {
        if (!item.isString())
            return false;
        out << item.toString();
    }
    return true;
}

Field jobBool(const char *name, FieldClass cls, bool PostingJobOptions::*m, bool required = false)
{
    return { name, cls, required, [m](const PersistedQueueOptions &o) {
        return QJsonValue(o.job.*m);
    }, [m](PersistedQueueOptions &o, const QJsonValue &v) {
        if (!v.isBool())
            return false;
        o.job.*m = v.toBool();
        return true;
    } };
}

Field jobUInt(const char *name, FieldClass cls, uint PostingJobOptions::*m, uint max)
{
    return { name, cls, false, [m](const PersistedQueueOptions &o) {
        return QJsonValue(static_cast<qint64>(o.job.*m));
    }, [m, max](PersistedQueueOptions &o, const QJsonValue &v) {
        if (!integralIn(v, 0, max))
            return false;
        o.job.*m = static_cast<uint>(v.toInteger());
        return true;
    } };
}

Field jobString(const char *name,
                FieldClass cls,
                QString PostingJobOptions::*m,
                bool required = false)
{
    return { name, cls, required, [m](const PersistedQueueOptions &o) {
        return QJsonValue(o.job.*m);
    }, [m](PersistedQueueOptions &o, const QJsonValue &v) {
        if (!v.isString())
            return false;
        o.job.*m = v.toString();
        return true;
    } };
}

Field optString(const char *name, FieldClass cls, QString PersistedQueueOptions::*m)
{
    return { name, cls, false, [m](const PersistedQueueOptions &o) {
        return QJsonValue(o.*m);
    }, [m](PersistedQueueOptions &o, const QJsonValue &v) {
        if (!v.isString())
            return false;
        o.*m = v.toString();
        return true;
    } };
}

QJsonValue sourcesToJson(const PersistedQueueOptions &o)
{
    QJsonArray array;
    for (const PersistedSource &s : o.sources)
        array.append(QJsonObject{ { QStringLiteral("path"), s.path },
                                  { QStringLiteral("dir"), s.isDir },
                                  { QStringLiteral("size"), s.sizeBytes },
                                  { QStringLiteral("mtime"), s.mtimeEpoch } });
    return array;
}

bool sourcesFromJson(PersistedQueueOptions &o, const QJsonValue &v)
{
    if (!v.isArray())
        return false;
    o.sources.clear();
    o.job.files.clear();
    for (const QJsonValue item : v.toArray()) {
        const QJsonObject obj = item.toObject();
        if (!item.isObject() || !obj.value(QStringLiteral("path")).isString()
            || !obj.value(QStringLiteral("dir")).isBool()
            || !integralIn(obj.value(QStringLiteral("size")), 0, 9.0e15)
            || !integralIn(obj.value(QStringLiteral("mtime")), -9.0e15, 9.0e15))
            return false;
        PersistedSource s;
        s.path = obj.value(QStringLiteral("path")).toString();
        s.isDir = obj.value(QStringLiteral("dir")).toBool();
        s.sizeBytes = obj.value(QStringLiteral("size")).toInteger();
        s.mtimeEpoch = obj.value(QStringLiteral("mtime")).toInteger();
        o.sources << s;
        o.job.files << QFileInfo(s.path);
    }
    return !o.sources.isEmpty();
}

QJsonValue metaToJson(const PersistedQueueOptions &o)
{
    QJsonObject obj;
    for (auto it = o.job.meta.cbegin(); it != o.job.meta.cend(); ++it)
        obj.insert(it.key(),
                   QJsonObject{ { QStringLiteral("value"), it.value().value },
                                { QStringLiteral("nzb"), it.value().scope == MetaScope::Nzb } });
    return obj;
}

bool metaFromJson(PersistedQueueOptions &o, const QJsonValue &v)
{
    if (!v.isObject())
        return false;
    o.job.meta.clear();
    const QJsonObject obj = v.toObject();
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        const QJsonObject entry = it.value().toObject();
        if (!it.value().isObject() || !entry.value(QStringLiteral("value")).isString()
            || !entry.value(QStringLiteral("nzb")).isBool())
            return false;
        o.job.meta.insert(it.key(),
                          MetaValue(entry.value(QStringLiteral("value")).toString(),
                                    entry.value(QStringLiteral("nzb")).toBool()
                                        ? MetaScope::Nzb
                                        : MetaScope::Local));
    }
    return true;
}

std::vector<Field> recipeFields()
{
    std::vector<Field> f;
    f.push_back({ "sources", FieldClass::ArtifactRecipe, true, sourcesToJson, sourcesFromJson });
    f.push_back(
        { "inputPaths", FieldClass::ArtifactRecipe, false, [](const PersistedQueueOptions &o) {
        return QJsonValue(toArray(o.job.inputPaths));
    }, [](PersistedQueueOptions &o, const QJsonValue &v) {
        return toStringList(v, o.job.inputPaths);
    } });
    f.push_back(
        jobBool("doCompress", FieldClass::ArtifactRecipe, &PostingJobOptions::doCompress, true));
    f.push_back(jobBool("doPar2", FieldClass::ArtifactRecipe, &PostingJobOptions::doPar2, true));
    f.push_back(
        optString("archiver", FieldClass::ArtifactRecipe, &PersistedQueueOptions::archiverFamily));
    f.push_back(jobString("rarArgs", FieldClass::ArtifactRecipe, &PostingJobOptions::rarArgs));
    f.push_back(jobString("rarName", FieldClass::ArtifactRecipe, &PostingJobOptions::rarName));
    f.push_back(
        jobUInt("rarSize", FieldClass::ArtifactRecipe, &PostingJobOptions::rarSize, 1u << 30));
    f.push_back(jobBool("useRarMax", FieldClass::ArtifactRecipe, &PostingJobOptions::useRarMax));
    f.push_back(jobUInt("rarMax", FieldClass::ArtifactRecipe, &PostingJobOptions::rarMax, 100000));
    f.push_back(jobUInt("par2Pct", FieldClass::ArtifactRecipe, &PostingJobOptions::par2Pct, 1000));
    f.push_back(
        optString("par2Tool", FieldClass::ArtifactRecipe, &PersistedQueueOptions::par2Family));
    f.push_back(
        jobString("par2Arguments", FieldClass::ArtifactRecipe, &PostingJobOptions::par2Arguments));
    f.push_back(jobBool("passwordGenerated",
                        FieldClass::ArtifactRecipe,
                        &PostingJobOptions::passwordGenerated));
    f.push_back(
        { "passwordOrigin", FieldClass::ArtifactRecipe, false, [](const PersistedQueueOptions &o) {
        return QJsonValue(passwordOriginName(o.passwordOrigin));
    }, [](PersistedQueueOptions &o, const QJsonValue &v) {
        return v.isString() && parsePasswordOrigin(v.toString(), o.passwordOrigin);
    } });
    f.push_back(
        { "passwordLength", FieldClass::ArtifactRecipe, false, [](const PersistedQueueOptions &o) {
        return QJsonValue(o.passwordLength);
    }, [](PersistedQueueOptions &o, const QJsonValue &v) {
        if (!integralIn(v, 0, 255))
            return false;
        o.passwordLength = v.toInt();
        return true;
    } });
    f.push_back(jobBool("obfuscateFileName",
                        FieldClass::ArtifactRecipe,
                        &PostingJobOptions::obfuscateFileName));
    f.push_back(
        jobUInt("lengthName", FieldClass::ArtifactRecipe, &PostingJobOptions::lengthName, 255));
    return f;
}

std::vector<Field> transferFields()
{
    std::vector<Field> f;
    f.push_back({ "articleSizeBytes",
                  FieldClass::TransferGeometry,
                  true,
                  [](const PersistedQueueOptions &o) { return QJsonValue(o.job.articleSizeBytes); },
                  [](PersistedQueueOptions &o, const QJsonValue &v) {
        if (!integralIn(v, 1, 1 << 30))
            return false;
        o.job.articleSizeBytes = v.toInteger();
        return true;
    } });
    f.push_back(jobBool("obfuscateArticles",
                        FieldClass::TransferGeometry,
                        &PostingJobOptions::obfuscateArticles));
    f.push_back({ "from", FieldClass::TransferGeometry, false, [](const PersistedQueueOptions &o) {
        return QJsonValue(QString::fromStdString(o.job.from));
    }, [](PersistedQueueOptions &o, const QJsonValue &v) {
        if (!v.isString())
            return false;
        o.job.from = v.toString().toStdString();
        return true;
    } });
    f.push_back({ "groups", FieldClass::TransferGeometry, true, [](const PersistedQueueOptions &o) {
        return QJsonValue(toArray(o.job.grpList));
    }, [](PersistedQueueOptions &o, const QJsonValue &v) {
        QStringList groups;
        if (!toStringList(v, groups) || groups.isEmpty())
            return false;
        o.job.grpList = groups;
        return true;
    } });
    return f;
}

std::vector<Field> outputAndOperationFields()
{
    std::vector<Field> f;
    f.push_back(
        jobString("nzbFilePath", FieldClass::Outputs, &PostingJobOptions::nzbFilePath, true));
    f.push_back({ "meta", FieldClass::Outputs, false, metaToJson, metaFromJson });
    f.push_back(
        jobBool("writePostInfoFile", FieldClass::Outputs, &PostingJobOptions::writePostInfoFile));
    f.push_back(
        jobString("postInfoTemplate", FieldClass::Outputs, &PostingJobOptions::postInfoTemplate));
    f.push_back(
        jobString("postInfoOutput", FieldClass::Outputs, &PostingJobOptions::postInfoOutput));
    f.push_back(jobBool("keepRar", FieldClass::Operation, &PostingJobOptions::keepRar));
    f.push_back(
        jobBool("delFilesAfterPost", FieldClass::Operation, &PostingJobOptions::delFilesAfterPost));
    f.push_back(jobBool("overwriteNzb", FieldClass::Operation, &PostingJobOptions::overwriteNzb));
    return f;
}

const std::vector<Field> &fields()
{
    static const std::vector<Field> all = [] {
        std::vector<Field> v = recipeFields();
        for (auto &&part : { transferFields(), outputAndOperationFields() })
            v.insert(v.end(), part.begin(), part.end());
        return v;
    }();
    return all;
}

PersistedSource sourceOf(const QFileInfo &file)
{
    QFileInfo fresh(file.absoluteFilePath());
    PersistedSource s;
    s.path = fresh.absoluteFilePath();
    s.isDir = fresh.isDir();
    s.sizeBytes = s.isDir ? 0 : fresh.size();
    s.mtimeEpoch = fresh.exists() ? fresh.lastModified().toSecsSinceEpoch() : 0;
    return s;
}

//! Freezes the families effectively used, or explains why it cannot.
QString freezeFamilies(PersistedQueueOptions &o)
{
    if (o.job.doCompress && o.archiverFamily.isEmpty()) {
        if (o.job.rarTool.isEmpty())
            return QStringLiteral("archiver_unresolved");
        o.archiverFamily = o.job.rarTool;
    }
    if (o.job.doPar2 && o.par2Family.isEmpty()) {
        if (o.job.par2Tool == par2::Tool::Auto)
            return QStringLiteral("par2_unresolved");
        o.par2Family = par2::toolName(o.job.par2Tool);
    }
    return {};
}

} // namespace

QString passwordOriginName(PasswordOrigin origin)
{
    switch (origin) {
    case PasswordOrigin::Absent: return QStringLiteral("absent");
    case PasswordOrigin::Fixed: return QStringLiteral("fixed");
    case PasswordOrigin::Generated: return QStringLiteral("generated");
    case PasswordOrigin::Provided: return QStringLiteral("provided");
    case PasswordOrigin::Declared: return QStringLiteral("declared");
    }
    return {};
}

bool parsePasswordOrigin(const QString &text, PasswordOrigin &origin)
{
    for (PasswordOrigin o : { PasswordOrigin::Absent,
                              PasswordOrigin::Fixed,
                              PasswordOrigin::Generated,
                              PasswordOrigin::Provided,
                              PasswordOrigin::Declared }) {
        if (passwordOriginName(o) == text) {
            origin = o;
            return true;
        }
    }
    return false;
}

namespace optionscodec
{

bool archiverArgsArePersistable(const QString &family, const QString &args)
{
    // Known options without a secret, per family. Anything else, including a
    // password switch in any spelling, keeps the arguments out of the queue.
    static const QRegularExpression rar(
        QStringLiteral(
            "^-(m[0-5]|ma[45]|md\\d+[kmg]?|ep[0-3]?|r-?|s-?|k|t|htb|htc|rr\\d+[%p]?"
            "|v\\d+[kmgb]?|mt\\d+|x\\S+|n\\S+|ai|dh|df|y|o[+-]|ol|id[cdnpq]+|ts[mca]?\\d?)$"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression sevenZip(
        QStringLiteral(
            "^-(mx=?\\d?|mhe=(on|off)|mhc=(on|off)|mmt=?(\\d+|on|off)?|ms=(on|off|\\d+[bkmgf]?)"
            "|m0=\\S+|mf=(on|off)|t(7z|zip|tar)|v\\d+[bkmg]?|r-?|ssw|bs[ope][012]|bb[0-3]?|y"
            "|x[!@r]?\\S+|i[!@r]?\\S+|sdel|stl|snl|sni|spf2?)$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression *allowed = family == QLatin1String("7zip") ? &sevenZip : &rar;
    if (family != QLatin1String("7zip") && family != QLatin1String("rar"))
        return args.trimmed().isEmpty();
    for (const QString &token : QProcess::splitCommand(args)) {
        if (token.startsWith(QLatin1String("-p"), Qt::CaseInsensitive)
            || token.startsWith(QLatin1String("-hp"), Qt::CaseInsensitive)
            || !allowed->match(token).hasMatch())
            return false;
    }
    return true;
}

EncodeResult encode(PersistedQueueOptions options)
{
    EncodeResult result;
    result.error = freezeFamilies(options);
    if (!result.ok())
        return result;
    // Sources are captured once, at admission; later encodings keep them.
    if (options.sources.isEmpty())
        for (const QFileInfo &file : options.job.files)
            options.sources << sourceOf(file);
    if (options.sources.isEmpty()) {
        result.error = QStringLiteral("no_source");
        return result;
    }
    result.omittedFields = options.omittedFields;
    if (!archiverArgsArePersistable(options.archiverFamily, options.job.rarArgs)) {
        options.job.rarArgs.clear();
        if (!result.omittedFields.contains(QStringLiteral("rarArgs")))
            result.omittedFields << QStringLiteral("rarArgs");
    }
    result.json.insert(QStringLiteral("v"), kVersion);
    for (const Field &f : fields())
        result.json.insert(QLatin1String(f.name), f.get(options));
    if (result.omittedFields.contains(QStringLiteral("rarArgs")))
        result.json.remove(QStringLiteral("rarArgs"));
    result.json.insert(QStringLiteral("omitted"), toArray(result.omittedFields));
    return result;
}

DecodeResult decode(const QJsonObject &json)
{
    DecodeResult result;
    if (json.value(QStringLiteral("v")).toInt(-1) != kVersion) {
        result.error = QStringLiteral("unsupported_version");
        return result;
    }
    for (const Field &f : fields()) {
        const QJsonValue v = json.value(QLatin1String(f.name));
        if (v.isUndefined()) {
            if (f.required)
                result.error = QStringLiteral("missing_field:%1").arg(QLatin1String(f.name));
        } else if (!f.set(result.options, v)) {
            result.error = QStringLiteral("invalid_field:%1").arg(QLatin1String(f.name));
        }
        if (!result.ok())
            return result;
    }
    if (!toStringList(json.value(QStringLiteral("omitted")), result.options.omittedFields)) {
        result.error = QStringLiteral("invalid_field:omitted");
        return result;
    }
    PersistedQueueOptions &o = result.options;
    o.job.rarTool = o.archiverFamily;
    if (!o.par2Family.isEmpty() && !par2::parseTool(o.par2Family, o.job.par2Tool))
        result.error = QStringLiteral("invalid_field:par2Tool");
    return result;
}

QStringList persistedFields()
{
    QStringList names;
    for (const Field &f : fields())
        names << QLatin1String(f.name);
    return names;
}

FieldClass fieldClass(const QString &field, bool *known)
{
    if (known)
        *known = true;
    if (field == QLatin1String("rarPass"))
        return FieldClass::ArtifactRecipe;
    if (field == QLatin1String("declaredPassword"))
        return FieldClass::Outputs;
    for (const Field &f : fields())
        if (field == QLatin1String(f.name))
            return f.cls;
    if (known)
        *known = false;
    return FieldClass::ArtifactRecipe;
}

QSet<FieldClass> changedClasses(const PersistedQueueOptions &before,
                                const PersistedQueueOptions &after)
{
    QSet<FieldClass> changed;
    for (const Field &f : fields())
        if (f.get(before) != f.get(after))
            changed.insert(f.cls);
    if (before.job.rarPass != after.job.rarPass)
        changed.insert(FieldClass::ArtifactRecipe);
    if (before.job.declaredPassword != after.job.declaredPassword)
        changed.insert(FieldClass::Outputs);
    return changed;
}

} // namespace optionscodec
