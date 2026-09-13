// Copyright (C) 2026 Hydro74000. GPL-3.0-or-later.
#include "Par2Settings.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSet>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <cmath>
#include <climits>
#include <limits>

namespace par2 {
QVector<Device> openClDevices(const QByteArray &output)
{
    QVector<Device> result;
    const auto platforms = QJsonDocument::fromJson(output).object().value("platforms").toArray();
    for (int p = 0; p < platforms.size(); ++p) {
        const auto devices = platforms[p].toObject().value("devices").toArray();
        for (int d = 0; d < devices.size(); ++d) {
            const auto device = devices[d].toObject();
            if (device.value("available").toBool() && device.value("supported").toBool()
                && device.value("type").toString().compare("gpu", Qt::CaseInsensitive) == 0)
                result << Device{QString("%1:%2").arg(p).arg(d), device.value("name").toString()};
        }
    }
    return result;
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
    if (path.contains(QStringLiteral("parpar"), Qt::CaseInsensitive)) return Tool::ParPar;
    if (path.contains(QStringLiteral("par2j"), Qt::CaseInsensitive)) return Tool::MultiPar;
    return Tool::Par2cmdline;
}
QString findExecutable(Tool tool)
{
    if (tool == Tool::Auto) {
        for (auto kind : {Tool::ParPar, Tool::Par2cmdline, Tool::MultiPar}) {
            const auto path = findExecutable(kind);
            if (!path.isEmpty()) return path;
        }
        return {};
    }
    QStringList names;
    if (tool == Tool::ParPar) names << QStringLiteral("parpar");
    else if (tool == Tool::Par2cmdline) names << QStringLiteral("par2");
    else {
#ifdef Q_OS_WIN
        names << QStringLiteral("par2j64") << QStringLiteral("par2j");
#else
        return {};
#endif
    }
    for (QString name : names) {
#ifdef Q_OS_WIN
        name += QStringLiteral(".exe");
#endif
        const QString bundled = QDir(QCoreApplication::applicationDirPath()).filePath(name);
        if (QFileInfo(bundled).isFile() && QFileInfo(bundled).isExecutable()) return bundled;
        const auto path = QStandardPaths::findExecutable(name);
        if (!path.isEmpty()) return path;
    }
    return {};
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
static qint64 byteSize(QString text)
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
Settings Settings::read(Tool kind, const QString &arguments)
{
    Settings s;
    s.tool = kind;
    s.originalArguments = arguments;
    if (arguments.trimmed().isEmpty()) {
        if (kind != Tool::MultiPar) s.memory = 1024;
        if (kind == Tool::Par2cmdline) s.volumes = Volumes::LargestInput;
        return s;
    }
    const auto args = QProcess::splitCommand(arguments);
    bool autoScale = false;
    int multiParPerFile = -1; //!< /lr<n>: blocks per recovery file, resolved below; -1 = absent
    QString parparMaximum;
    QSet<QString> seen;
    auto once = [&](const QString &category) {
        if (seen.contains(category)) s.custom = true;
        seen.insert(category);
    };
    auto number = [&](const QString &value) {
        bool ok = false;
        const int result = value.toInt(&ok);
        if (!ok) s.custom = true;
        return result;
    };
    for (int i = 0; i < args.size(); ++i) {
        QString a = args[i], value;
        auto take = [&](const QString &shortName, const QString &longName = QString()) {
            if (!longName.isEmpty() && a.startsWith(longName + QLatin1Char('='))) {
                value = a.mid(longName.size() + 1); return true;
            }
            if (a == shortName || (!longName.isEmpty() && a == longName)) {
                if (i + 1 < args.size()) value = args[++i];
                return true;
            }
            if (!shortName.isEmpty() && a.startsWith(shortName) && a.size() > shortName.size()) {
                value = a.mid(shortName.size()); return true;
            }
            return false;
        };
        if (a == QStringLiteral("c") || a == QStringLiteral("create")) {
            if (kind == Tool::ParPar) s.custom = true;
            once("command");
            continue;
        }
        if (kind == Tool::MultiPar) {
            a = a.toLower();
            if (a.startsWith("/rr")) continue;
            if (a.startsWith("/ss")) { once("blocks"); s.blocks = Blocks::Size; s.blockBytes = byteSize(a.mid(3)); }
            else if (a.startsWith("/sn")) { once("blocks"); s.blocks = Blocks::Count; s.blockCount = number(a.mid(3)); }
            else if (a.startsWith("/rd")) {
                once("distribution");
                const QString v = a.mid(3);
                if (v == "0") s.distribution = Distribution::Equal;
                else if (v == "2") s.distribution = Distribution::PowersOfTwo;
                else if (v == "3") s.distribution = Distribution::Decimal;
                else s.custom = true;
            } else if (a == "/lr" || a == "/lr0") { once("volumes"); s.volumes = Volumes::LargestInput; }
            else if (a.startsWith("/lr")) {
                once("volumes"); multiParPerFile = number(a.mid(3));
                if (multiParPerFile < 1) s.custom = true;
            }
            else if (a.startsWith("/rf")) { once("volumes"); s.volumes = Volumes::Count; s.volumeCount = number(a.mid(3)); }
            else if (a.startsWith("/lc")) {
                once("threads");
                bool ok; const uint v = a.mid(3).toUInt(&ok);
                s.threads = int(v & 255); s.gpu = (v & 256) != 0;
                if (!ok || (v & ~511u) || s.threads > 32) s.custom = true;
            } else if (a.startsWith("/m")) { once("memory"); s.memory = number(a.mid(2)); }
            else s.custom = true;
        } else if (kind == Tool::ParPar) {
            if (a == "--auto-slice-size" || a == "-S") { once("auto-size"); autoScale = true; continue; }
            if (take(QString(), "--opencl-process")) {
                once("gpu");
                s.gpu = true;
                // Partial CPU/GPU allocations remain a custom policy.
                if (value != "100%") s.custom = true;
                continue;
            }
            if (take(QString(), "--max-input-slices")) { once("max-size"); parparMaximum = value; continue; }
            if (take("-s", "--input-slices")) {
                once("blocks");
                if (QRegularExpression("[BKMGTbkmgt]$").match(value).hasMatch()) {
                    s.blocks = Blocks::Size; s.blockBytes = byteSize(value);
                } else { s.blocks = Blocks::Count; s.blockCount = number(value); }
            } else if (take("-r", "--recovery-slices")) { /* replaced by the post's percentage */ }
            else if (take("-m", "--memory")) {
                once("memory");
                const auto bytes = byteSize(value);
                if (bytes < 0 || bytes % 1048576 || bytes / 1048576 > INT_MAX) s.custom = true;
                else s.memory = int(bytes / 1048576);
            } else if (take("-t", "--threads")) { once("threads"); s.threads = number(value); }
            else if (take("-d", "--slice-dist")) {
                once("distribution");
                if (value == "equal") s.distribution = Distribution::Equal;
                else if (value == "uniform") s.distribution = Distribution::Uniform;
                else if (value == "pow2") s.distribution = Distribution::PowersOfTwo;
                else s.custom = true;
            } else if (take("-p", "--slices-per-file")) {
                once("volumes");
                if (value == "1l") s.volumes = Volumes::LargestInput;
                else if (value.startsWith('<')) {
                    s.volumes = Volumes::Size; s.volumeBytes = byteSize(value.mid(1));
                } else s.custom = true;
            } else if (take("-F", "--recovery-files")) { once("volumes"); s.volumes = Volumes::Count; s.volumeCount = number(value); }
            else if (take(QString(), "--opencl-device")) s.device = value;
            else s.custom = true;
        } else {
            if (a == "-l") { once("volumes"); s.volumes = Volumes::LargestInput; }
            else if (a == "-u") { once("distribution"); s.distribution = Distribution::Uniform; }
            else if (take("-s")) { once("blocks"); s.blocks = Blocks::Size; s.blockBytes = byteSize(value); }
            else if (take("-b")) { once("blocks"); s.blocks = Blocks::Count; s.blockCount = number(value); }
            else if (take("-r")) { }
            else if (take("-n")) { once("volumes"); s.volumes = Volumes::Count; s.volumeCount = number(value); }
            else if (take("-m")) { once("memory"); s.memory = number(value); }
            else if (take("-t")) { once("threads"); s.threads = number(value); }
            else s.custom = true;
        }
    }
    // "/lr<n>" is a number of blocks per recovery file, so it only maps back to
    // a target size when "/ss" fixed the block size in the same command line.
    if (multiParPerFile >= 0) {
        if (multiParPerFile > 0 && s.blocks == Blocks::Size && s.blockBytes >= 4
            && s.blockBytes <= std::numeric_limits<qint64>::max() / multiParPerFile) {
            s.volumes = Volumes::Size;
            s.volumeBytes = s.blockBytes * multiParPerFile;
        } else
            s.custom = true; // nothing to convert: keep the line as it was written
    }
    if (autoScale) {
        if (s.blocks == Blocks::Size && s.blockBytes == 1048576) s.blocks = Blocks::Automatic;
        else s.custom = true; // preserve adaptive custom slice policies verbatim
    }
    if (!parparMaximum.isEmpty()) {
        if (s.blocks != Blocks::Size || byteSize(parparMaximum) != s.blockBytes) s.custom = true;
    }
    // Legacy ParPar size arguments acquire --auto-slice-size during execution.
    // They cannot be advertised as an exact size unless a strict limit exists.
    if (kind == Tool::ParPar && s.blocks == Blocks::Size && parparMaximum.isEmpty()) s.custom = true;
    if (!s.device.isEmpty() && !s.gpu) s.custom = true;
    if (!s.validate().isEmpty()) s.custom = true;
    return s;
}
QString Settings::validate() const
{
    if (custom) return {};
    if (blocks == Blocks::Size && (blockBytes < 4 || blockBytes % 4 || blockBytes >= 2147483648LL))
        return QCoreApplication::translate("Par2Settings", "Block size must be a multiple of 4 bytes, below 2 GiB.");
    if (blocks == Blocks::Count && (blockCount < 1 || blockCount > 32768))
        return QCoreApplication::translate("Par2Settings", "The source block count must be between 1 and 32768.");
    if (volumes == Volumes::Size && (volumeBytes < 4
        || (tool == Tool::MultiPar && blocks == Blocks::Size && volumeBytes / blockBytes > INT_MAX)))
        return QCoreApplication::translate("Par2Settings", "Choose a positive volume size below the tool's limit.");
    if (volumes == Volumes::Count && (volumeCount < 1 || volumeCount > 65535))
        return QCoreApplication::translate("Par2Settings", "The recovery volume count must be between 1 and 65535.");
    // par2cmdline 1.x enforces this ("the maximum allowed recovery file count
    // is 31"); 0.8.x had no cap, but 1.4.0 is what every package now ships.
    if (tool == Tool::Par2cmdline && volumes == Volumes::Count && volumeCount > 31)
        return QCoreApplication::translate("Par2Settings", "par2cmdline creates at most 31 recovery volumes.");
    if (tool == Tool::Par2cmdline && (volumes == Volumes::Size || distribution == Distribution::Equal
        || distribution == Distribution::Decimal || (volumes == Volumes::LargestInput && distribution == Distribution::Uniform)))
        return QCoreApplication::translate("Par2Settings", "This combination is not supported by par2cmdline.");
    if (tool == Tool::ParPar && distribution == Distribution::Decimal)
        return QCoreApplication::translate("Par2Settings", "Decimal distribution requires MultiPar.");
    if (tool == Tool::MultiPar && volumes == Volumes::Size
        && (blocks != Blocks::Size || volumeBytes < blockBytes))
        return QCoreApplication::translate("Par2Settings", "MultiPar limits a recovery file by block count: "
                                                           "set an exact block size (Advanced) no larger than the target size.");
    if (tool == Tool::MultiPar && (distribution == Distribution::Uniform
        || (volumes == Volumes::Count && distribution != Distribution::Equal && distribution != Distribution::Automatic)))
        return QCoreApplication::translate("Par2Settings", "MultiPar supports a volume count only with equal distribution.");
    if (threads < 0 || (tool == Tool::MultiPar && threads > 32) || memory < 0 || (tool == Tool::MultiPar && memory > 7))
        return QCoreApplication::translate("Par2Settings", "CPU or memory setting exceeds the tool's supported range.");
    return {};
}
QStringList Settings::arguments(uint redundancy) const
{
    QStringList args;
    if (tool == Tool::MultiPar) {
        args << "c" << QString("/rr%1").arg(redundancy);
        if (blocks == Blocks::Size) args << QString("/ss%1").arg(blockBytes);
        else if (blocks == Blocks::Count) args << QString("/sn%1").arg(blockCount);
        if (distribution == Distribution::Equal) args << "/rd0";
        else if (distribution == Distribution::PowersOfTwo) args << "/rd2";
        else if (distribution == Distribution::Decimal) args << "/rd3";
        if (volumes == Volumes::LargestInput) args << "/lr";
        else if (volumes == Volumes::Size) {
            // par2j: "/lr<n> ... This is the max number of blocks in a file, not
            // max size of file itself". Converting needs the block size, which
            // validate() demands for this combination. Keep /ls out of guided
            // arguments: generic /ls splits sources; the special /ls2 + /lr
            // byte-limit mode remains available through custom arguments.
            const qint64 perFile = blockBytes > 0 ? volumeBytes / blockBytes : 0;
            args << QString("/lr%1").arg(qMax<qint64>(1, perFile));
        }
        else if (volumes == Volumes::Count) args << QString("/rf%1").arg(volumeCount);
        if (threads || gpu) args << QString("/lc%1").arg(threads + (gpu ? 256 : 0));
        if (memory) args << QString("/m%1").arg(memory);
    } else if (tool == Tool::ParPar) {
        args << QString("-r%1%").arg(redundancy);
        if (blocks == Blocks::Automatic) args << "-s1M" << "--auto-slice-size";
        else if (blocks == Blocks::Size)
            args << QString("-s%1B").arg(blockBytes) << QString("--max-input-slices=%1B").arg(blockBytes);
        else args << QString("-s%1").arg(blockCount);
        if (distribution == Distribution::Equal) args << "-d" << "equal";
        else if (distribution == Distribution::Uniform) args << "-d" << "uniform";
        else if (distribution == Distribution::PowersOfTwo) args << "-d" << "pow2";
        if (volumes == Volumes::LargestInput) args << "-p1l";
        else if (volumes == Volumes::Size) args << QString("-p<%1B").arg(volumeBytes);
        else if (volumes == Volumes::Count) args << QString("-F%1").arg(volumeCount);
        if (memory) args << QString("-m%1M").arg(memory);
        if (threads) args << QString("-t%1").arg(threads);
        if (gpu) {
            // The CLI requires a value even though its help calls it optional.
            args << "--opencl-process=100%";
            if (!device.isEmpty()) args << "--opencl-device" << device;
        }
    } else {
        args << "c" << QString("-r%1").arg(redundancy);
        if (blocks == Blocks::Size) args << QString("-s%1").arg(blockBytes);
        else if (blocks == Blocks::Count) args << QString("-b%1").arg(blockCount);
        if (distribution == Distribution::Uniform) args << "-u";
        if (volumes == Volumes::LargestInput) args << "-l";
        else if (volumes == Volumes::Count) args << QString("-n%1").arg(volumeCount);
        if (memory) args << QString("-m%1").arg(memory);
        if (threads) args << QString("-t%1").arg(threads);
    }
    return args;
}
qint64 Settings::exactBlockBytes() const
{
    return !custom && blocks == Blocks::Size ? blockBytes : 0;
}
Estimate Settings::estimate(const QVector<qint64> &sizes, uint redundancy) const
{
    Estimate e;
    if (custom || !validate().isEmpty() || sizes.isEmpty()) return e;
    qint64 largest = 0;
    for (qint64 size : sizes) {
        if (size < 0 || e.sourceBytes > std::numeric_limits<qint64>::max() - size) return e;
        e.sourceBytes += size;
        largest = qMax(largest, size);
    }
    if (!e.sourceBytes) return e;
    // No supported block policy can describe more than this. Reject before
    // converting an arbitrary source total through floating point to qint64.
    if (e.sourceBytes > 32768LL * 2147483644LL) return e;
    auto align = [](qint64 n) { return ((n + 3) / 4) * 4; };
    e.blockBytes = blockBytes;
    if (blocks == Blocks::Count) e.blockBytes = align(qMax<qint64>(4, qint64(std::ceil(double(e.sourceBytes) / blockCount))));
    if (blocks == Blocks::Automatic) {
        if (tool == Tool::ParPar) e.blockBytes = qMax<qint64>(1048576, qint64(std::ceil(double(e.sourceBytes) / (32768.0 * 1048576))) * 1048576);
        else if (tool == Tool::Par2cmdline) e.blockBytes = align(qMax<qint64>(4, qint64(std::ceil(double(e.sourceBytes) / 2000))));
        else e.blockBytes = align(qMax<qint64>(qint64(std::ceil(10 * std::sqrt(double(e.sourceBytes)))),
                                                               qint64(std::ceil(double(e.sourceBytes) / 3000))));
    }
    for (auto size : sizes) e.sourceBlocks += size / e.blockBytes + (size % e.blockBytes != 0);
    if (e.sourceBlocks > 32768) return e;
    const double recovery = e.sourceBlocks * redundancy / 100.0;
    // Creation tools use different rounding rules, including on tiny posts.
    if (tool == Tool::MultiPar) e.recoveryBlocks = qint64(std::floor(recovery));
    else if (tool == Tool::Par2cmdline) e.recoveryBlocks = qint64(std::floor(recovery + 0.5));
    else e.recoveryBlocks = qint64(std::ceil(recovery));
    if (redundancy > 0) e.recoveryBlocks = qMax<qint64>(1, e.recoveryBlocks);
    if (e.recoveryBlocks > 65535 || e.blockBytes >= 2147483648LL) return e;
    e.recoveryBytes = e.recoveryBlocks * e.blockBytes;
    e.valid = true;
    if (!e.recoveryBlocks) { e.volumeCount = 0; e.largestRecoveryBytes = 0; return e; }
    if (volumes == Volumes::Count) {
        if (volumeCount > e.recoveryBlocks) return e;
        e.volumeCount = volumeCount;
        // pow2 + a specified file count is tool-dependent: do not invent a maximum.
        if (distribution == Distribution::Equal || distribution == Distribution::Uniform
            || (tool == Tool::MultiPar && distribution == Distribution::Automatic))
            e.largestRecoveryBytes = ((e.recoveryBlocks + e.volumeCount - 1) / e.volumeCount) * e.blockBytes;
        return e;
    }
    if (distribution == Distribution::Decimal || (tool == Tool::MultiPar && volumes == Volumes::Automatic))
        return e;
    qint64 cap = e.recoveryBlocks;
    if (volumes == Volumes::Size) cap = volumeBytes / e.blockBytes;
    else if (volumes == Volumes::LargestInput) cap = (largest + e.blockBytes - 1) / e.blockBytes;
    if (cap <= 0) { e.valid = false; return e; }
    bool powers = distribution == Distribution::PowersOfTwo
        || (distribution == Distribution::Automatic && tool != Tool::MultiPar);
    qint64 remaining = e.recoveryBlocks, next = powers ? 1 : cap;
    e.volumeCount = 0; e.largestRecoveryBytes = 0;
    while (remaining > 0) {
        const qint64 count = qMin(remaining, qMin(cap, next));
        remaining -= count; ++e.volumeCount;
        e.largestRecoveryBytes = qMax(e.largestRecoveryBytes, count * e.blockBytes);
        if (powers) next = qMin(cap, next * 2);
    }
    return e;
}
}
