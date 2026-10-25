#include "SystemInfo.h"
#include <windows.h>
#include <sysinfoapi.h>
#include <QString>

SystemInfoData SystemInfo::collect() {
    SystemInfoData info;

    // OS 信息
    OSVERSIONINFOEXW osvi = {0};
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    if (GetVersionExW((OSVERSIONINFOW*)&osvi)) {
        info.osVersion = QString("%1.%2.%3").arg(osvi.dwMajorVersion).arg(osvi.dwMinorVersion).arg(osvi.dwBuildNumber);
        info.osBuild = QString::number(osvi.dwBuildNumber);
        if (osvi.dwMajorVersion == 10 && osvi.dwMinorVersion == 0) {
            info.osName = "Windows 10/11";
        } else if (osvi.dwMajorVersion == 6 && osvi.dwMinorVersion == 3) {
            info.osName = "Windows 8.1";
        } else if (osvi.dwMajorVersion == 6 && osvi.dwMinorVersion == 2) {
            info.osName = "Windows 8";
        } else if (osvi.dwMajorVersion == 6 && osvi.dwMinorVersion == 1) {
            info.osName = "Windows 7";
        } else {
            info.osName = QString("Windows %1.%2").arg(osvi.dwMajorVersion).arg(osvi.dwMinorVersion);
        }
    }

    // CPU 信息
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    info.cpuCores = si.dwNumberOfProcessors;
    info.cpuThreads = si.dwNumberOfProcessors;

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        WCHAR cpuName[256] = {0};
        DWORD size = sizeof(cpuName);
        if (RegQueryValueExW(hKey, L"ProcessorNameString", NULL, NULL, (LPBYTE)cpuName, &size) == ERROR_SUCCESS) {
            info.cpuName = QString::fromWCharArray(cpuName).trimmed();
        }
        RegCloseKey(hKey);
    }

    // 内存信息
    MEMORYSTATUSEX memStatus;
    memStatus.dwLength = sizeof(memStatus);
    if (GlobalMemoryStatusEx(&memStatus)) {
        info.totalMemoryMB = memStatus.ullTotalPhys / (1024 * 1024);
        info.availableMemoryMB = memStatus.ullAvailPhys / (1024 * 1024);
    }

    // GPU 信息（通过注册表）
    info.gpuName = "Unknown";
    info.gpuMemoryMB = 0;
    for (int i = 0; i < 4; i++) {
        QString keyPath = QString("HARDWARE\\DEVICEMAP\\VIDEO");
        HKEY videoKey;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\VIDEO", 0, KEY_READ, &videoKey) == ERROR_SUCCESS) {
            WCHAR deviceName[256] = {0};
            DWORD deviceSize = sizeof(deviceName);
            QString valueName = QString("\\Device\\Video%1").arg(i);
            if (RegQueryValueExW(videoKey, valueName.toStdWString().c_str(), NULL, NULL, (LPBYTE)deviceName, &deviceSize) == ERROR_SUCCESS) {
                // deviceName 是 \Registry\Machine\System\... 格式，需要转换
                QString devStr = QString::fromWCharArray(deviceName);
                QString regPath = devStr.replace("\\Registry\\Machine\\", "");
                HKEY gpuKey;
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, regPath.toStdWString().c_str(), 0, KEY_READ, &gpuKey) == ERROR_SUCCESS) {
                    WCHAR gpuName[256] = {0};
                    DWORD gpuSize = sizeof(gpuName);
                    if (RegQueryValueExW(gpuKey, L"Device Description", NULL, NULL, (LPBYTE)gpuName, &gpuSize) == ERROR_SUCCESS) {
                        QString name = QString::fromWCharArray(gpuName).trimmed();
                        if (!name.isEmpty()) {
                            info.allGpus.append(name);
                            if (i == 0) info.gpuName = name;
                        }
                    }
                    // 显存信息
                    DWORD memSize = 0;
                    DWORD memSizeSize = sizeof(memSize);
                    if (RegQueryValueExW(gpuKey, L"HardwareInformation.qwMemorySize", NULL, NULL, (LPBYTE)&memSize, &memSizeSize) == ERROR_SUCCESS) {
                        if (i == 0) info.gpuMemoryMB = memSize / (1024 * 1024);
                    }
                    RegCloseKey(gpuKey);
                }
            }
            RegCloseKey(videoKey);
        }
    }

    return info;
}

QString SystemInfo::format(const SystemInfoData& info) {
    QString out;
    out += "=== System Information ===\n";
    out += QString("OS: %1 (Build %2)\n").arg(info.osName, info.osBuild);
    out += QString("CPU: %1 (%2 cores/%3 threads)\n").arg(info.cpuName).arg(info.cpuCores).arg(info.cpuThreads);
    out += QString("Memory: %1 MB total, %2 MB available\n").arg(info.totalMemoryMB).arg(info.availableMemoryMB);
    out += QString("Primary GPU: %1 (%2 MB)\n").arg(info.gpuName).arg(info.gpuMemoryMB);
    if (info.allGpus.size() > 1) {
        out += "All GPUs:\n";
        for (const QString& gpu : info.allGpus) {
            out += QString("  - %1\n").arg(gpu);
        }
    }
    return out;
}
