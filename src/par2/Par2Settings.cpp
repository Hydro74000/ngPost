// Copyright (C) 2026 Hydro74000. GPL-3.0-or-later.
#include "Par2Settings.h"
#include "tools/ExternalToolResolver.h"
#include <QCoreApplication>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <cmath>
#include <climits>
#include <limits>

namespace par2 {
//! par2j: "/ls2 ... setting limit size must be less than 2 GB". Decimal, so
//! that the value also stays inside the int the option is parsed into.
static constexpr qint64 sMultiParSizeLimit = 2000000000LL;
QVector<Device> openClDevices(const QByteArray &output, bool gpuOnly)
{
    QVector<Device> result;
    const auto platforms = QJsonDocument::fromJson(output).object().value("platforms").toArray();
    for (int p = 0; p < platforms.size(); ++p) {
        const auto devices = platforms[p].toObject().value("devices").toArray();
        for (int d = 0; d < devices.size(); ++d) {
            const auto device = devices[d].toObject();
            if (device.value("available").toBool() && device.value("supported").toBool()
                && (!gpuOnly || device.value("type").toString().compare("gpu", Qt::CaseInsensitive) == 0))
                result << Device{QString("%1:%2").arg(p).arg(d), device.value("name").toString()};
        }
    }
    return result;
}
int openClDeviceCount(const QByteArray &output)
{
    const auto platforms = QJsonDocument::fromJson(output).object().value("platforms");
    if (!platforms.isArray()) return -1;
    for (const auto &platform : platforms.toArray())
        if (!platform.toObject().value("devices").isArray()) return -1;
    return openClDevices(output, false).size();
}
QString toolName(Tool tool)
{
    switch (tool) {
    case Tool::ParPar: return QStringLiteral("parpar");
    case Tool::Par2cmdline: return QStringLiteral("par2cmdline");
    case Tool::MultiPar: return QStringLiteral("multipar");
    default: return QStringLiteral("auto");
    }
}
bool parseTool(const QString &name, Tool &tool)
{
    for (Tool value : {Tool::Auto, Tool::ParPar, Tool::Par2cmdline, Tool::MultiPar})
        if (name.trimmed().compare(toolName(value), Qt::CaseInsensitive) == 0) {
            tool = value;
            return true;
        }
    return false;
}
Tool detectTool(const QString &path)
{
    if (QFileInfo(path).fileName().contains(QStringLiteral("parpar"), Qt::CaseInsensitive))
        return Tool::ParPar;
    if (QFileInfo(path).fileName().contains(QStringLiteral("par2j"), Qt::CaseInsensitive))
        return Tool::MultiPar;
    return Tool::Par2cmdline;
}
QString findExecutable(Tool tool)
{
    return externaltool::resolve(toolName(tool)).path;
}
QString joinArguments(const QStringList &args)
{
    QStringList quoted;
    for (QString arg : args) {
        // QProcess::splitCommand uses triple quotes for a literal double quote.
        arg.replace(QLatin1Char('"'), QStringLiteral("\"\"\""));
        if (arg.contains(QRegularExpression(QStringLiteral("\\s"))))
            arg = QLatin1Char('"') + arg + QLatin1Char('"');
        quoted << arg;
    }
    return quoted.join(QLatin1Char(' '));
}
static qint64 byteSize(const QString &text)
{
    const auto match = QRegularExpression(QStringLiteral("^(\\d+)([BKMGT]?)$"),
                                         QRegularExpression::CaseInsensitiveOption).match(text);
    if (!match.hasMatch()) return -1;
    bool ok = false;
    qint64 value = match.captured(1).toLongLong(&ok);
    int power = QStringLiteral("BKMGT").indexOf(match.captured(2).toUpper());
    if (!ok || value <= 0) return -1;
    while (power-- > 0) {
        if (value > std::numeric_limits<qint64>::max() / 1024) return -1;
        value *= 1024;
    }
    return value;
}
namespace
{
//! One pass over a PAR2 command line. The per-tool readers share the cursor,
//! since an option may consume the argument that follows it.
class ArgumentReader
{
public:
    ArgumentReader(Settings &settings, const QString &commandLine)
        : s(settings)
        , args(QProcess::splitCommand(commandLine))
    {
    }
    void read()
    {
        for (i = 0; i < args.size(); ++i) {
            a = args[i];
            value.clear();
            if (a == QStringLiteral("c") || a == QStringLiteral("create")) {
                if (s.tool == Tool::ParPar)
                    s.custom = true;
                once("command");
                continue;
            }
            if (s.tool == Tool::MultiPar)
                readMultiPar();
            else if (s.tool == Tool::ParPar)
                readParPar();
            else
                readPar2cmdline();
        }
        resolveMultiParLimit();
        resolveSliceHints();
    }

private:
    void once(const QString &category)
    {
        if (seen.contains(category)) s.custom = true;
        seen.insert(category);
    }
    int number(const QString &text)
    {
        bool ok = false;
        const int result = text.toInt(&ok);
        if (!ok) s.custom = true;
        return result;
    }
    bool take(const QString &shortName, const QString &longName = QString())
    {
        if (!longName.isEmpty() && a.startsWith(longName + QLatin1Char('='))) {
            value = a.mid(longName.size() + 1);
            return true;
        }
        if (a == shortName || (!longName.isEmpty() && a == longName)) {
            if (i + 1 < args.size())
                value = args[++i];
            return true;
        }
        if (!shortName.isEmpty() && a.startsWith(shortName) && a.size() > shortName.size()) {
            value = a.mid(shortName.size());
            return true;
        }
        return false;
    }

    void readMultiPar()
    {
        a = a.toLower();
        if (a.startsWith("/rr"))
            return;
        if (a.startsWith("/ss")) {
            once("blocks");
            s.blocks = Blocks::Size;
            s.blockBytes = byteSize(a.mid(3));
        } else if (a.startsWith("/sn")) {
            once("blocks");
            s.blocks = Blocks::Count;
            s.blockCount = number(a.mid(3));
        } else if (a.startsWith("/rd"))
            readMultiParDistribution(a.mid(3));
        else if (a == "/lr" || a == "/lr0") {
            once("volumes");
            s.volumes = Volumes::LargestInput;
        } else if (a.startsWith("/lr")) {
            once("volumes");
            multiParPerFile = number(a.mid(3));
            if (multiParPerFile < 1)
                s.custom = true;
        } else if (a.startsWith("/rf")) {
            once("volumes");
            s.volumes = Volumes::Count;
            s.volumeCount = number(a.mid(3));
        } else if (a == "/ls2") {
            once("split");
            multiParByteLimit = true;
        } else if (a.startsWith("/lc"))
            readMultiParThreads(a.mid(3));
        else if (a.startsWith("/m")) {
            once("memory");
            s.memory = number(a.mid(2));
        } else
            s.custom = true;
    }
    void readMultiParDistribution(const QString &v)
    {
        once("distribution");
        if (v == "0")
            s.distribution = Distribution::Equal;
        else if (v == "2")
            s.distribution = Distribution::PowersOfTwo;
        else if (v == "3")
            s.distribution = Distribution::Decimal;
        else
            s.custom = true;
    }
    void readMultiParThreads(const QString &text)
    {
        once("threads");
        bool ok;
        const uint v = text.toUInt(&ok);
        s.threads = int(v & 255);
        s.gpu = (v & 256) != 0;
        if (!ok || (v & ~511u) || s.threads > 32)
            s.custom = true;
    }

    void readParPar()
    {
        if (a == "--auto-slice-size" || a == "-S") {
            once("auto-size");
            autoScale = true;
            return;
        }
        if (take(QString(), "--opencl-process")) {
            once("gpu");
            s.gpu = true;
            // Partial CPU/GPU allocations remain a custom policy.
            if (value != "100%")
                s.custom = true;
            return;
        }
        if (take(QString(), "--max-input-slices")) {
            once("max-size");
            parparMaximum = value;
            return;
        }
        if (take("-s", "--input-slices"))
            readParParSlices();
        else if (take("-r", "--recovery-slices")) { /* replaced by the post's percentage */
        } else if (take("-m", "--memory"))
            readParParMemory();
        else if (take("-t", "--threads")) {
            once("threads");
            s.threads = number(value);
        } else if (take("-d", "--slice-dist"))
            readParParDistribution();
        else if (take("-p", "--slices-per-file"))
            readParParVolumes();
        else if (take("-F", "--recovery-files")) {
            once("volumes");
            s.volumes = Volumes::Count;
            s.volumeCount = number(value);
        } else if (take(QString(), "--opencl-device"))
            s.device = value;
        else
            s.custom = true;
    }
    void readParParSlices()
    {
        once("blocks");
        if (QRegularExpression("[BKMGTbkmgt]$").match(value).hasMatch()) {
            s.blocks = Blocks::Size;
            s.blockBytes = byteSize(value);
        } else {
            s.blocks = Blocks::Count;
            s.blockCount = number(value);
        }
    }
    void readParParMemory()
    {
        once("memory");
        const auto bytes = byteSize(value);
        if (bytes < 0 || bytes % 1048576 || bytes / 1048576 > INT_MAX)
            s.custom = true;
        else
            s.memory = int(bytes / 1048576);
    }
    void readParParDistribution()
    {
        once("distribution");
        if (value == "equal")
            s.distribution = Distribution::Equal;
        else if (value == "uniform")
            s.distribution = Distribution::Uniform;
        else if (value == "pow2")
            s.distribution = Distribution::PowersOfTwo;
        else
            s.custom = true;
    }
    void readParParVolumes()
    {
        once("volumes");
        if (value == "1l")
            s.volumes = Volumes::LargestInput;
        else if (value.startsWith('<')) {
            s.volumes = Volumes::Size;
            s.volumeBytes = byteSize(value.mid(1));
        } else
            s.custom = true;
    }

    void readPar2cmdline()
    {
        if (a == "-l") {
            once("volumes");
            s.volumes = Volumes::LargestInput;
        } else if (a == "-u") {
            once("distribution");
            s.distribution = Distribution::Uniform;
        } else if (take("-s")) {
            once("blocks");
            s.blocks = Blocks::Size;
            s.blockBytes = byteSize(value);
        } else if (take("-b")) {
            once("blocks");
            s.blocks = Blocks::Count;
            s.blockCount = number(value);
        } else if (take("-r")) {
        } else if (take("-n")) {
            once("volumes");
            s.volumes = Volumes::Count;
            s.volumeCount = number(value);
        } else if (take("-m")) {
            once("memory");
            s.memory = number(value);
        } else if (take("-t")) {
            once("threads");
            s.threads = number(value);
        } else
            s.custom = true;
    }

    void resolveMultiParLimit()
    {
        // "/lr<n>" counts blocks per recovery file, unless "/ls2" turned it into a
        // byte limit. /ss is only a request: MultiPar can adjust the slice size,
        // so a block-count limit cannot be translated to bytes without loss.
        if (multiParByteLimit && multiParPerFile >= 4 && multiParPerFile < sMultiParSizeLimit) {
            s.volumes = Volumes::Size;
            s.volumeBytes = multiParPerFile;
        } else if (multiParByteLimit || multiParPerFile >= 0) {
            // No guided meaning: "/ls2" with a byte limit outside the guided range
            // or with "/lr0", and a block-count limit, which cannot become bytes.
            s.custom = true;
        }
    }
    void resolveSliceHints()
    {
        if (autoScale) {
            if (s.blocks == Blocks::Size && s.blockBytes == 1048576)
                s.blocks = Blocks::Automatic;
            else
                s.custom = true; // preserve adaptive custom slice policies verbatim
        }
        if (!parparMaximum.isEmpty()) {
            if (s.blocks != Blocks::Size || byteSize(parparMaximum) != s.blockBytes)
                s.custom = true;
        }
        // Legacy ParPar size arguments acquire --auto-slice-size during execution.
        // They cannot be advertised as an exact size unless a strict limit exists.
        if (s.tool == Tool::ParPar && s.blocks == Blocks::Size && parparMaximum.isEmpty())
            s.custom = true;
        if (!s.device.isEmpty() && !s.gpu)
            s.custom = true;
        if (!s.validate().isEmpty())
            s.custom = true;
    }

    Settings &s;
    const QStringList args;
    int i = 0;
    QString a, value;
    bool autoScale = false;
    int multiParPerFile = -1;       //!< /lr<n>, resolved at the end; -1 = absent
    bool multiParByteLimit = false; //!< /ls2 turns that /lr<n> into a byte limit
    QString parparMaximum;
    QSet<QString> seen;
};
}
Settings Settings::parse(Tool kind, const QString &arguments)
{
    Settings s;
    s.tool = kind;
    s.originalArguments = arguments;
    if (arguments.trimmed().isEmpty()) {
        if (kind != Tool::MultiPar)
            s.memory = 1024;
        if (kind == Tool::Par2cmdline)
            s.volumes = Volumes::LargestInput;
        return s;
    }
    ArgumentReader(s, arguments).read();
    return s;
}
static QString blockError(const Settings &s)
{
    if (s.blocks == Blocks::Size
        && (s.blockBytes < 4 || s.blockBytes % 4 || s.blockBytes >= 2147483648LL))
        return QCoreApplication::translate("Par2Settings", "Block size must be a multiple of 4 bytes, below 2 GiB.");
    if (s.blocks == Blocks::Count && (s.blockCount < 1 || s.blockCount > 32768))
        return QCoreApplication::translate("Par2Settings", "The source block count must be between 1 and 32768.");
    return { };
}
static QString volumeError(const Settings &s)
{
    if (s.volumes == Volumes::Size
        && (s.volumeBytes < 4
            || (s.tool == Tool::MultiPar && s.blocks == Blocks::Size
                && s.volumeBytes / s.blockBytes > INT_MAX)))
        return QCoreApplication::translate("Par2Settings", "Choose a positive volume size below the tool's limit.");
    if (s.volumes == Volumes::Count && (s.volumeCount < 1 || s.volumeCount > 65535))
        return QCoreApplication::translate("Par2Settings", "The recovery volume count must be between 1 and 65535.");
    return { };
}
static QString par2cmdlineError(const Settings &s)
{
    // par2cmdline 1.x enforces this ("the maximum allowed recovery file count
    // is 31"); 0.8.x had no cap, but 1.4.0 is what every package now ships.
    if (s.volumes == Volumes::Count && s.volumeCount > 31)
        return QCoreApplication::translate("Par2Settings", "par2cmdline creates at most 31 recovery volumes.");
    if (s.volumes == Volumes::Size || s.distribution == Distribution::Equal
        || s.distribution == Distribution::Decimal
        || (s.volumes == Volumes::LargestInput && s.distribution == Distribution::Uniform))
        return QCoreApplication::translate("Par2Settings", "This combination is not supported by par2cmdline.");
    return { };
}
static QString multiParError(const Settings &s)
{
    if (s.volumes == Volumes::Size && s.volumeBytes >= sMultiParSizeLimit)
        return QCoreApplication::translate("Par2Settings", "MultiPar size targets must be below 2 GB (2000000000 bytes). "
                                                           "Use custom arguments for limits expressed in blocks.");
    if (s.distribution == Distribution::Uniform
        || (s.volumes == Volumes::Count && s.distribution != Distribution::Equal
            && s.distribution != Distribution::Automatic))
        return QCoreApplication::translate("Par2Settings", "MultiPar supports a volume count only with equal distribution.");
    return { };
}
static QString toolError(const Settings &s)
{
    if (s.tool == Tool::Par2cmdline)
        return par2cmdlineError(s);
    if (s.tool == Tool::ParPar && s.distribution == Distribution::Decimal)
        return QCoreApplication::translate("Par2Settings",
                                           "Decimal distribution requires MultiPar.");
    if (s.tool == Tool::MultiPar)
        return multiParError(s);
    return { };
}
static QString resourceError(const Settings &s)
{
    const bool multiPar = s.tool == Tool::MultiPar;
    if (s.threads < 0 || (multiPar && s.threads > 32) || s.memory < 0 || (multiPar && s.memory > 7))
        return QCoreApplication::translate("Par2Settings", "CPU or memory setting exceeds the tool's supported range.");
    return {};
}
QString Settings::validate() const
{
    if (custom)
        return { };
    // The first failing check wins, in the order the dialog reports them.
    for (auto check : { blockError, volumeError, toolError, resourceError }) {
        const QString error = check(*this);
        if (!error.isEmpty())
            return error;
    }
    return { };
}
static void appendMultiParArguments(const Settings &s, QStringList &args)
{
    if (s.blocks == Blocks::Size)
        args << QString("/ss%1").arg(s.blockBytes);
    else if (s.blocks == Blocks::Count)
        args << QString("/sn%1").arg(s.blockCount);
    if (s.distribution == Distribution::Equal)
        args << "/rd0";
    else if (s.distribution == Distribution::PowersOfTwo)
        args << "/rd2";
    else if (s.distribution == Distribution::Decimal)
        args << "/rd3";
    if (s.volumes == Volumes::LargestInput)
        args << "/lr";
    else if (s.volumes == Volumes::Size) {
        // par2j: "/lr<n> ... This is the max number of blocks in a file, not
        // max size of file itself", except that "/ls2 has a special feature
        // to set limit size of recovery files directly. When both /ls2 and
        // /lr(limit size) are set, setting number of /lr is recognizned as
        // file size instead of number of blocks. In this usage, setting
        // limit size must be less than 2 GB." /ls2 is a mode flag, not a
        // split size: it leaves the sources alone (checked on par2j 1.3.3.5).
        args << "/ls2" << QString("/lr%1").arg(s.volumeBytes);
    } else if (s.volumes == Volumes::Count)
        args << QString("/rf%1").arg(s.volumeCount);
    if (s.threads || s.gpu)
        args << QString("/lc%1").arg(s.threads + (s.gpu ? 256 : 0));
    if (s.memory)
        args << QString("/m%1").arg(s.memory);
}
static void appendParParArguments(const Settings &s, QStringList &args)
{
    if (s.blocks == Blocks::Automatic)
        args << "-s1M" << "--auto-slice-size";
    else if (s.blocks == Blocks::Size)
        args << QString("-s%1B").arg(s.blockBytes)
             << QString("--max-input-slices=%1B").arg(s.blockBytes);
    else
        args << QString("-s%1").arg(s.blockCount);
    if (s.distribution == Distribution::Equal)
        args << "-d" << "equal";
    else if (s.distribution == Distribution::Uniform)
        args << "-d" << "uniform";
    else if (s.distribution == Distribution::PowersOfTwo)
        args << "-d" << "pow2";
    if (s.volumes == Volumes::LargestInput)
        args << "-p1l";
    else if (s.volumes == Volumes::Size)
        args << QString("-p<%1B").arg(s.volumeBytes);
    else if (s.volumes == Volumes::Count)
        args << QString("-F%1").arg(s.volumeCount);
    if (s.memory)
        args << QString("-m%1M").arg(s.memory);
    if (s.threads)
        args << QString("-t%1").arg(s.threads);
    if (s.gpu) {
        // The CLI requires a value even though its help calls it optional.
        args << "--opencl-process=100%";
        if (!s.device.isEmpty())
            args << "--opencl-device" << s.device;
    }
}
static void appendPar2cmdlineArguments(const Settings &s, QStringList &args)
{
    if (s.blocks == Blocks::Size)
        args << QString("-s%1").arg(s.blockBytes);
    else if (s.blocks == Blocks::Count)
        args << QString("-b%1").arg(s.blockCount);
    if (s.distribution == Distribution::Uniform)
        args << "-u";
    if (s.volumes == Volumes::LargestInput)
        args << "-l";
    else if (s.volumes == Volumes::Count)
        args << QString("-n%1").arg(s.volumeCount);
    if (s.memory)
        args << QString("-m%1").arg(s.memory);
    if (s.threads)
        args << QString("-t%1").arg(s.threads);
}
QStringList Settings::arguments(uint redundancy) const
{
    QStringList args;
    if (tool == Tool::MultiPar) {
        args << "c" << QString("/rr%1").arg(redundancy);
        appendMultiParArguments(*this, args);
    } else if (tool == Tool::ParPar) {
        args << QString("-r%1%").arg(redundancy);
        appendParParArguments(*this, args);
    } else {
        args << "c" << QString("-r%1").arg(redundancy);
        appendPar2cmdlineArguments(*this, args);
    }
    return args;
}
qint64 Settings::exactBlockBytes() const
{
    // MultiPar clamps /ss to the source-dependent slice range. Without the
    // actual archive inputs its requested size is never a guaranteed hint.
    return !custom && tool != Tool::MultiPar && blocks == Blocks::Size ? blockBytes : 0;
}
//! Adds the sizes up in place; false on a negative size or an overflow.
static bool sumSources(const QVector<qint64> &sizes, qint64 &total, qint64 &largest)
{
    for (qint64 size : sizes) {
        if (size < 0 || total > std::numeric_limits<qint64>::max() - size)
            return false;
        total += size;
        largest = qMax(largest, size);
    }
    return true;
}
static qint64 alignTo4(qint64 n)
{
    return ((n + 3) / 4) * 4;
}
static qint64 automaticBlockBytes(Tool tool, qint64 sourceBytes)
{
    const double total = double(sourceBytes);
    if (tool == Tool::ParPar)
        return qMax<qint64>(1048576, qint64(std::ceil(total / (32768.0 * 1048576))) * 1048576);
    if (tool == Tool::Par2cmdline)
        return alignTo4(qMax<qint64>(4, qint64(std::ceil(total / 2000))));
    return alignTo4(
        qMax<qint64>(qint64(std::ceil(10 * std::sqrt(total))), qint64(std::ceil(total / 3000))));
}
static qint64 estimatedBlockBytes(const Settings &s, qint64 sourceBytes, qint64 largest)
{
    qint64 bytes = s.blockBytes;
    if (s.blocks == Blocks::Count)
        bytes = alignTo4(qMax<qint64>(4, qint64(std::ceil(double(sourceBytes) / s.blockCount))));
    if (s.blocks == Blocks::Automatic)
        bytes = automaticBlockBytes(s.tool, sourceBytes);
    if (s.tool == Tool::MultiPar)
        bytes = qMin(bytes, alignTo4(largest));
    return bytes;
}
static qint64 estimatedRecoveryBlocks(Tool tool, qint64 sourceBlocks, uint redundancy)
{
    const double recovery = sourceBlocks * redundancy / 100.0;
    qint64 blocks = 0;
    // Creation tools use different rounding rules, including on tiny posts.
    if (tool == Tool::MultiPar)
        blocks = qint64(std::floor(recovery));
    else if (tool == Tool::Par2cmdline)
        blocks = qint64(std::floor(recovery + 0.5));
    else
        blocks = qint64(std::ceil(recovery));
    if (redundancy > 0)
        blocks = qMax<qint64>(1, blocks);
    return blocks;
}
static void estimateCountedVolumes(const Settings &s, Estimate &e)
{
    if (s.volumeCount > e.recoveryBlocks)
        return;
    e.volumeCount = s.volumeCount;
    // pow2 + a specified file count is tool-dependent: do not invent a maximum.
    if (s.distribution == Distribution::Equal || s.distribution == Distribution::Uniform
        || (s.tool == Tool::MultiPar && s.distribution == Distribution::Automatic))
        e.largestRecoveryBytes = ((e.recoveryBlocks + e.volumeCount - 1) / e.volumeCount)
            * e.blockBytes;
}
//! Fills the volumes with at most cap blocks each, doubling from one when powers.
static void distributeVolumes(Estimate &e, qint64 cap, bool powers)
{
    qint64 remaining = e.recoveryBlocks, next = powers ? 1 : cap;
    e.volumeCount = 0; e.largestRecoveryBytes = 0;
    while (remaining > 0) {
        const qint64 count = qMin(remaining, qMin(cap, next));
        remaining -= count; ++e.volumeCount;
        e.largestRecoveryBytes = qMax(e.largestRecoveryBytes, count * e.blockBytes);
        if (powers) next = qMin(cap, next * 2);
    }
}
static void estimateVolumes(const Settings &s, Estimate &e, qint64 largest)
{
    if (!e.recoveryBlocks) {
        e.volumeCount = 0;
        e.largestRecoveryBytes = 0;
        return;
    }
    if (s.volumes == Volumes::Count) {
        estimateCountedVolumes(s, e);
        return;
    }
    if (s.distribution == Distribution::Decimal
        || (s.tool == Tool::MultiPar && s.volumes == Volumes::Automatic))
        return;
    qint64 cap = e.recoveryBlocks;
    if (s.volumes == Volumes::Size)
        cap = s.volumeBytes / e.blockBytes;
    else if (s.volumes == Volumes::LargestInput)
        cap = (largest + e.blockBytes - 1) / e.blockBytes;
    if (s.tool == Tool::MultiPar)
        cap = qMax<qint64>(1, cap);
    if (cap <= 0) {
        e.valid = false;
        return;
    }
    const bool powers = s.distribution == Distribution::PowersOfTwo
        || (s.distribution == Distribution::Automatic && s.tool != Tool::MultiPar);
    distributeVolumes(e, cap, powers);
}
Estimate Settings::estimate(const QVector<qint64> &sizes, uint redundancy) const
{
    Estimate e;
    if (custom || !validate().isEmpty() || sizes.isEmpty())
        return e;
    qint64 largest = 0;
    if (!sumSources(sizes, e.sourceBytes, largest) || !e.sourceBytes)
        return e;
    // No supported block policy can describe more than this. Reject before
    // converting an arbitrary source total through floating point to qint64.
    if (e.sourceBytes > 32768LL * 2147483644LL)
        return e;
    e.blockBytes = estimatedBlockBytes(*this, e.sourceBytes, largest);
    for (auto size : sizes)
        e.sourceBlocks += size / e.blockBytes + (size % e.blockBytes != 0);
    if (e.sourceBlocks > 32768)
        return e;
    e.recoveryBlocks = estimatedRecoveryBlocks(tool, e.sourceBlocks, redundancy);
    if (e.recoveryBlocks > 65535 || e.blockBytes >= 2147483648LL)
        return e;
    e.recoveryBytes = e.recoveryBlocks * e.blockBytes;
    e.valid = true;
    estimateVolumes(*this, e, largest);
    return e;
}
}
