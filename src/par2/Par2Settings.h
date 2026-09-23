// Copyright (C) 2026 Hydro74000. GPL-3.0-or-later.
#ifndef PAR2SETTINGS_H
#define PAR2SETTINGS_H

#include <QStringList>
#include <QVector>

namespace par2 {
enum class Tool { Auto, ParPar, Par2cmdline, MultiPar };
enum class Blocks { Automatic, Size, Count };
enum class Volumes { Automatic, LargestInput, Size, Count };
enum class Distribution { Automatic, Equal, Uniform, PowersOfTwo, Decimal };

QString toolName(Tool tool);
bool parseTool(const QString &name, Tool &tool);
Tool detectTool(const QString &path);
QString findExecutable(Tool tool);
QString joinArguments(const QStringList &args);
struct Device { QString id; QString name; };
QVector<Device> openClDevices(const QByteArray &output, bool gpuOnly = true);
//! Available, supported devices, including CPUs; -1 for an invalid listing.
int openClDeviceCount(const QByteArray &output);

struct Estimate {
    qint64 sourceBytes = 0;
    qint64 blockBytes = 0;
    qint64 sourceBlocks = 0;
    qint64 recoveryBlocks = 0;
    qint64 recoveryBytes = 0;
    qint64 volumeCount = -1;
    qint64 largestRecoveryBytes = -1;
    bool valid = false;
};

struct Settings {
    Tool tool = Tool::Par2cmdline;
    Blocks blocks = Blocks::Automatic;
    qint64 blockBytes = 1048576;
    int blockCount = 2000;
    Volumes volumes = Volumes::Automatic;
    qint64 volumeBytes = 250LL * 1048576;
    int volumeCount = 10;
    Distribution distribution = Distribution::Automatic;
    int threads = 0;
    int memory = 0; // MiB for ParPar/par2cmdline; eighths of free RAM for MultiPar.
    bool gpu = false;
    QString device;
    bool custom = false;
    QString originalArguments;

    static Settings parse(Tool tool, const QString &arguments);
    QString validate() const;
    QStringList arguments(uint redundancy) const;
    qint64 exactBlockBytes() const;
    Estimate estimate(const QVector<qint64> &sizes, uint redundancy) const;
};
}
#endif
