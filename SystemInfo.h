#ifndef SYSTEMINFO_H
#define SYSTEMINFO_H

#include <QString>
#include <QStringList>

struct SystemInfoData {
    QString osName;
    QString osVersion;
    QString osBuild;
    QString cpuName;
    int cpuCores;
    int cpuThreads;
    quint64 totalMemoryMB;
    quint64 availableMemoryMB;
    QString gpuName;
    QString gpuDriverVersion;
    quint64 gpuMemoryMB;
    QStringList allGpus;
};

class SystemInfo {
public:
    static SystemInfoData collect();
    static QString format(const SystemInfoData& info);
};

#endif // SYSTEMINFO_H
