#include "DiagnosticPanel.h"
#include "PlayerController.h"
#include "Logger.h"
#include "SystemInfo.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QTabWidget>
#include <QTextEdit>
#include <QLabel>
#include <QComboBox>
#include <QPushButton>
#include <QFileInfo>
#include <QScrollBar>
#include <QDateTime>
#include <windows.h>
#include <psapi.h>

#pragma comment(lib, "psapi.lib")

DiagnosticPanel::DiagnosticPanel(PlayerController* player, QWidget* parent)
    : QDialog(parent), m_player(player), m_lastLogCount(0)
{
    setWindowTitle("MediaPlayer 诊断面板");
    resize(800, 600);
    setupUI();

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &DiagnosticPanel::onTimer);
    m_timer->start(500);
}

void DiagnosticPanel::setupUI() {
    QVBoxLayout* mainLayout = new QVBoxLayout(this);

    // 顶部工具栏
    QHBoxLayout* toolbar = new QHBoxLayout();
    toolbar->addWidget(new QLabel("日志级别:"));
    m_logLevelCombo = new QComboBox();
    m_logLevelCombo->addItems({"TRACE", "DEBUG", "INFO", "WARN", "ERROR"});
    m_logLevelCombo->setCurrentIndex(2); // INFO
    connect(m_logLevelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DiagnosticPanel::onLogLevelChanged);
    toolbar->addWidget(m_logLevelCombo);
    toolbar->addStretch();

    m_clearBtn = new QPushButton("清空日志");
    // m_logView 还未创建，connect 移到后面
    toolbar->addWidget(m_clearBtn);

    m_exportBtn = new QPushButton("导出诊断包");
    m_exportBtn->setStyleSheet("QPushButton{background:#0078d4;color:white;padding:4px 12px;border-radius:3px;}");
    connect(m_exportBtn, &QPushButton::clicked, this, &DiagnosticPanel::onExportClicked);
    toolbar->addWidget(m_exportBtn);
    mainLayout->addLayout(toolbar);

    // 状态标签区域
    QGridLayout* statusLayout = new QGridLayout();

    m_fileLabel = new QLabel("文件: -");
    m_fileLabel->setStyleSheet("font-weight:bold;");
    statusLayout->addWidget(m_fileLabel, 0, 0, 1, 2);

    m_resolutionLabel = new QLabel("分辨率: -");
    statusLayout->addWidget(m_resolutionLabel, 1, 0);
    m_positionLabel = new QLabel("位置: -");
    statusLayout->addWidget(m_positionLabel, 1, 1);

    m_fpsLabel = new QLabel("帧率: decode=- display=-");
    statusLayout->addWidget(m_fpsLabel, 2, 0);
    m_queueLabel = new QLabel("队列: vFrame=- aFrame=- vPkt=- aPkt=-");
    statusLayout->addWidget(m_queueLabel, 2, 1);

    m_d3d11Label = new QLabel("D3D11: -");
    statusLayout->addWidget(m_d3d11Label, 3, 0);
    m_clockDiffLabel = new QLabel("时钟: audio=- video=-");
    statusLayout->addWidget(m_clockDiffLabel, 3, 1);

    m_aiLabel = new QLabel("AI: -");
    statusLayout->addWidget(m_aiLabel, 4, 0);
    m_systemLabel = new QLabel("系统: -");
    statusLayout->addWidget(m_systemLabel, 4, 1);

    mainLayout->addLayout(statusLayout);

    // 日志视图
    m_logView = new QTextEdit();
    m_logView->setReadOnly(true);
    m_logView->setStyleSheet("QTextEdit{background:#1e1e1e;color:#d4d4d4;font-family:Consolas;font-size:11px;}");
    mainLayout->addWidget(m_logView, 1);

    // 现在 m_logView 已创建，可以连接信号
    connect(m_clearBtn, &QPushButton::clicked, m_logView, &QTextEdit::clear);
}

void DiagnosticPanel::onTimer() {
    refresh();
}

void DiagnosticPanel::refresh() {
    updatePlaybackInfo();
    updateDecodeInfo();
    updateD3D11Info();
    updateAIInfo();
    updateSystemInfo();
    updateLogView();
}

void DiagnosticPanel::updatePlaybackInfo() {
    if (!m_player) return;
    auto stats = m_player->diagnosticStats();

    QFileInfo fi(stats.currentFile);
    m_fileLabel->setText(QString("文件: %1").arg(fi.fileName()));

    if (stats.videoWidth > 0) {
        m_resolutionLabel->setText(QString("分辨率: %1x%2").arg(stats.videoWidth).arg(stats.videoHeight));
    }

    QString posStr = QDateTime::fromSecsSinceEpoch(stats.positionMs / 1000).toUTC().toString("HH:mm:ss");
    QString durStr = QDateTime::fromSecsSinceEpoch(stats.durationMs / 1000).toUTC().toString("HH:mm:ss");
    m_positionLabel->setText(QString("位置: %1 / %2").arg(posStr, durStr));
}

void DiagnosticPanel::updateDecodeInfo() {
    if (!m_player) return;
    auto stats = m_player->diagnosticStats();

    m_fpsLabel->setText(QString::asprintf("帧率: decode=%.1f display=%.1f",
        stats.decodeFps, stats.displayFps));

    m_queueLabel->setText(QString("队列: vFrame=%1 aFrame=%2 vPkt=%3 aPkt=%4")
        .arg(stats.videoFrameQueueSize)
        .arg(stats.audioFrameQueueSize)
        .arg(stats.videoPacketQueueSize)
        .arg(stats.audioPacketQueueSize));

    m_clockDiffLabel->setText(QString::asprintf("时钟: audio=%.3fs video=%.3fs diff=%.0fms",
        stats.audioClock, stats.videoClock, qAbs(stats.audioClock - stats.videoClock) * 1000));
}

void DiagnosticPanel::updateD3D11Info() {
    if (!m_player) return;
    auto stats = m_player->diagnosticStats();
    m_d3d11Label->setText(QString("D3D11: %1")
        .arg(stats.hwDecodeEnabled ? "D3D11VA 硬件解码" : "软件解码"));
}

void DiagnosticPanel::updateAIInfo() {
    // AI 状态可以从 WhisperTranscriber/Translator 获取，这里先显示基本信息
    m_aiLabel->setText("AI: ASR+MT 已集成 (T4.8)");
}

void DiagnosticPanel::updateSystemInfo() {
    // CPU 和内存使用
    MEMORYSTATUSEX memStatus;
    memStatus.dwLength = sizeof(memStatus);
    GlobalMemoryStatusEx(&memStatus);
    DWORD memUsedMB = (memStatus.ullTotalPhys - memStatus.ullAvailPhys) / (1024 * 1024);

    // 进程内存
    PROCESS_MEMORY_COUNTERS pmc;
    DWORD procMemMB = 0;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        procMemMB = pmc.WorkingSetSize / (1024 * 1024);
    }

    m_systemLabel->setText(QString("系统: 内存=%1MB/%2MB 进程=%3MB")
        .arg(memUsedMB)
        .arg(memStatus.ullTotalPhys / (1024 * 1024))
        .arg(procMemMB));
}

void DiagnosticPanel::updateLogView() {
    QStringList logs = Logger::instance().recentLogs(500);

    // 按级别过滤
    int minLevel = m_logLevelCombo->currentIndex();
    QStringList filtered;
    for (const QString& line : logs) {
        bool include = false;
        if (line.contains("[TRACE]") && minLevel <= 0) include = true;
        else if (line.contains("[DEBUG]") && minLevel <= 1) include = true;
        else if (line.contains("[INFO]") && minLevel <= 2) include = true;
        else if (line.contains("[WARN]") && minLevel <= 3) include = true;
        else if (line.contains("[ERROR]") && minLevel <= 4) include = true;
        else if (line.contains("[FATAL]")) include = true;
        if (include) filtered.append(line);
    }

    if (filtered.size() != m_lastLogCount) {
        m_logView->setPlainText(filtered.join("\n"));
        m_logView->verticalScrollBar()->setValue(m_logView->verticalScrollBar()->maximum());
        m_lastLogCount = filtered.size();
    }
}

void DiagnosticPanel::onLogLevelChanged(int index) {
    Q_UNUSED(index);
    m_lastLogCount = 0; // 强制刷新
    updateLogView();
}

void DiagnosticPanel::onExportClicked() {
    emit exportRequested();
}
