#include <QApplication>
#include "Version.h"
#include "AppPaths.h"
#include "RealtimeSubtitleWorker.h"
#include <QMainWindow>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSlider>
#include <QComboBox>
#include <QLabel>
#include <QFileDialog>
#include <QFileInfo>
#include <QListWidget>
#include <QMessageBox>
#include <QDialog>
#include <QTextEdit>
#include <QTimer>
#include <QShortcut>
#include <QCursor>
#include <QSettings>
#include <QPropertyAnimation>
#include <QMouseEvent>
#include <QDateTime>
#include <QDir>
#include <QScreen>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <functional>
#include <thread>
#include "VideoWidget.h"
#include "PlayerController.h"
#include "WhisperTranscriber.h"
#include "Translator.h"
#include "Logger.h"
#include "CrashHandler.h"
#include "DiagnosticPanel.h"
#include "DiagnosticExporter.h"

// 主窗口resize/move事件过滤器：同步定位独立UI窗口 + 拖拽打开文件
class MainWindowFilter : public QObject
{
public:
    MainWindowFilter(std::function<void()> cb, std::function<void(const QStringList&)> dropCb)
        : callback(cb), dropCallback(dropCb) {}
protected:
    bool eventFilter(QObject *obj, QEvent *event) override {
        if (event->type() == QEvent::Resize || event->type() == QEvent::Move) {
            callback();
        }
        if (event->type() == QEvent::DragEnter) {
            QDragEnterEvent *e = static_cast<QDragEnterEvent*>(event);
            if (e->mimeData()->hasUrls()) {
                e->acceptProposedAction();
                return true;
            }
        }
        if (event->type() == QEvent::Drop) {
            QDropEvent *e = static_cast<QDropEvent*>(event);
            QStringList files;
            for (const QUrl &url : e->mimeData()->urls()) {
                if (url.isLocalFile()) files << url.toLocalFile();
            }
            if (!files.isEmpty() && dropCallback) {
                dropCallback(files);
                e->acceptProposedAction();
                return true;
            }
        }
        return QObject::eventFilter(obj, event);
    }
private:
    std::function<void()> callback;
    std::function<void(const QStringList&)> dropCallback;
};

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // ===== T4.9 高效反馈系统初始化 =====
    // Logger 自动初始化（单例构造时创建日志文件）
    // CrashHandler 延迟安装：在D3D11初始化完成后再安装，避免捕获驱动层预期异常
    LOG_INFO(LogModule::CORE, "MediaPlayer starting, version v" MP_APP_VERSION);

    // 主窗口：只包含视频区域，避免Qt控件与D3D11原生窗口重绘冲突
    QMainWindow window;
    window.setWindowTitle("MediaPlayer");
    window.resize(960, 640);
    window.setAcceptDrops(true);  // 启用拖拽打开文件

    QWidget *central = new QWidget(&window);
    central->setStyleSheet("background-color: black;");
    QVBoxLayout *mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    VideoWidget *videoWindow = new VideoWidget();
    QWidget *videoWidget = QWidget::createWindowContainer(videoWindow, central);
    videoWidget->setStyleSheet("background-color: black;");
    mainLayout->addWidget(videoWidget, 1);
    window.setCentralWidget(central);

    const int CONTROL_HEIGHT = 50;
    const int SETTINGS_HEIGHT = 36;
    const int UI_TOTAL_HEIGHT = CONTROL_HEIGHT + SETTINGS_HEIGHT;

    // ===== 独立UI窗口：包含设置栏+控制栏，悬浮在主窗口上方 =====
    QWidget *uiWindow = new QWidget(&window,
        Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    uiWindow->setStyleSheet("background-color: rgba(30,30,30,240);");
    QVBoxLayout *uiLayout = new QVBoxLayout(uiWindow);
    uiLayout->setContentsMargins(0, 0, 0, 0);
    uiLayout->setSpacing(0);

    // 设置栏
    QWidget *settingsBar = new QWidget(uiWindow);
    settingsBar->setFixedHeight(SETTINGS_HEIGHT);
    QHBoxLayout *settingsLayout = new QHBoxLayout(settingsBar);
    settingsLayout->setContentsMargins(12, 0, 12, 0);
    settingsLayout->setSpacing(8);

    QLabel *hwLabel = new QLabel("硬件解码", settingsBar);
    hwLabel->setStyleSheet("color:#bbbbbb;font-size:12px;");
    QComboBox *hwCombo = new QComboBox(settingsBar);
    hwCombo->addItem("自动 (D3D11VA)", 1);
    hwCombo->addItem("关闭 (纯软件)", 0);
    hwCombo->setFixedWidth(140);
    hwCombo->setStyleSheet("QComboBox{color:white;background:#3a3a3a;border:none;padding:2px 6px;}QComboBox QAbstractItemView{color:white;background:#3a3a3a;selection-background-color:#0078d4;}");

    settingsLayout->addWidget(hwLabel);
    settingsLayout->addWidget(hwCombo);
    settingsLayout->addStretch(1);

    // 字幕开关（突出显示）
    QPushButton *subtitleToggle = new QPushButton("字幕", settingsBar);
    subtitleToggle->setFixedSize(56, 24);
    subtitleToggle->setStyleSheet("QPushButton{color:#cccccc;background:#3a3a3a;border:none;border-radius:3px;font-size:11px;}QPushButton:hover{background:#4a4a4a;}");
    subtitleToggle->setCheckable(true);
    settingsLayout->addWidget(subtitleToggle);

    // 选择字幕文件（精简小按钮）
    QPushButton *subtitleFileBtn = new QPushButton("...", settingsBar);
    subtitleFileBtn->setFixedSize(28, 24);
    subtitleFileBtn->setStyleSheet("QPushButton{color:#999999;background:#333333;border:none;border-radius:3px;font-size:12px;font-weight:bold;}QPushButton:hover{background:#444444;color:#cccccc;}");
    subtitleFileBtn->setToolTip("选择字幕文件");
    settingsLayout->addWidget(subtitleFileBtn);

    settingsLayout->addStretch();

    // 播放列表切换按钮
    QPushButton *playlistBtn = new QPushButton("列表", settingsBar);
    playlistBtn->setFixedSize(56, 24);
    playlistBtn->setStyleSheet("QPushButton{color:#cccccc;background:#3a3a3a;border:none;border-radius:3px;font-size:11px;}QPushButton:hover{background:#4a4a4a;}");
    playlistBtn->setCheckable(true);
    settingsLayout->addWidget(playlistBtn);

    // AI识别按钮（T4.8验证用）
    QPushButton *aiBtn = new QPushButton("AI识别", settingsBar);
    aiBtn->setFixedSize(64, 24);
    aiBtn->setStyleSheet("QPushButton{color:#cccccc;background:#3a3a3a;border:none;border-radius:3px;font-size:11px;}QPushButton:hover{background:#4a4a4a;}");
    aiBtn->setToolTip("开启/停止AI语音识别字幕");
    settingsLayout->addWidget(aiBtn);

    // 实时字幕：边播边出字（环形缓冲 -> VAD -> whisper -> 翻译）
    QPushButton *rtBtn = new QPushButton("实时字幕", settingsBar);
    rtBtn->setFixedSize(64, 24);
    rtBtn->setStyleSheet("QPushButton{color:#cccccc;background:#3a3a3a;border:none;border-radius:3px;font-size:11px;}QPushButton:hover{background:#4a4a4a;}");
    rtBtn->setToolTip("开启/停止实时字幕（播放中边播边识别）");
    settingsLayout->addWidget(rtBtn);

    // AI翻译目标语言选择（T4.8）
    QComboBox *translateLangCombo = new QComboBox(settingsBar);
    translateLangCombo->setFixedSize(72, 24);
    translateLangCombo->setStyleSheet("QComboBox{color:#cccccc;background:#3a3a3a;border:none;border-radius:3px;font-size:11px;padding-left:4px;}QComboBox::drop-down{border:none;width:16px;}QComboBox QAbstractItemView{background:#2a2a2a;color:#cccccc;selection-background-color:#0078d4;}");
    translateLangCombo->addItem("原文", "original");
    translateLangCombo->addItem("中文", "中文");
    translateLangCombo->addItem("英语", "英语");
    translateLangCombo->addItem("日语", "日语");
    translateLangCombo->addItem("韩语", "韩语");
    translateLangCombo->setToolTip("AI识别后翻译目标语言");
    settingsLayout->addWidget(translateLangCombo);

    // 关于按钮
    QPushButton *aboutBtn = new QPushButton("关于", settingsBar);
    aboutBtn->setFixedSize(56, 24);
    aboutBtn->setStyleSheet("QPushButton{color:#999999;background:#333333;border:none;border-radius:3px;font-size:11px;}QPushButton:hover{background:#444444;color:#cccccc;}");
    aboutBtn->setToolTip("关于本软件与开源许可");
    settingsLayout->addWidget(aboutBtn);

    // 可拖动字幕标签类
    class DraggableLabel : public QLabel {
    public:
        QPoint offset;  // 用户拖动偏移量
        QPoint dragStartMouse, dragStartWindow;
        bool dragging = false;
        std::function<void()> onDragEnd;  // 拖动结束回调，用于更新offset
        QString normalStyle;  // 保存原始样式（半透明）
        QString dragStyle;    // 拖动时样式（不透明，减少重绘）

        void mousePressEvent(QMouseEvent *e) override {
            if (e->button() == Qt::LeftButton) {
                dragging = true;
                dragStartMouse = e->globalPosition().toPoint();
                dragStartWindow = frameGeometry().topLeft();
                if (!dragStyle.isEmpty()) setStyleSheet(dragStyle);  // 拖动时不透明背景
            }
            QLabel::mousePressEvent(e);
        }
        void mouseMoveEvent(QMouseEvent *e) override {
            if (dragging) {
                QPoint delta = e->globalPosition().toPoint() - dragStartMouse;
                move(dragStartWindow + delta);
            }
            QLabel::mouseMoveEvent(e);
        }
        void mouseReleaseEvent(QMouseEvent *e) override {
            if (dragging) {
                dragging = false;
                if (!normalStyle.isEmpty()) setStyleSheet(normalStyle);  // 恢复半透明
                if (onDragEnd) onDragEnd();
            }
            QLabel::mouseReleaseEvent(e);
        }
    };

    DraggableLabel *subtitleLabel = new DraggableLabel();
    subtitleLabel->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    subtitleLabel->setAttribute(Qt::WA_TranslucentBackground);
    subtitleLabel->setAlignment(Qt::AlignCenter);
    subtitleLabel->setWordWrap(true);
    subtitleLabel->normalStyle =
        "QLabel{"
        "color:white;"
        "background:rgba(0,0,0,180);"
        "border-radius:4px;"
        "padding:6px 16px;"
        "font-size:18px;"
        "font-weight:bold;"
        "}";
    subtitleLabel->dragStyle =
        "QLabel{"
        "color:white;"
        "background:rgba(0,0,0,255);"
        "border-radius:4px;"
        "padding:6px 16px;"
        "font-size:18px;"
        "font-weight:bold;"
        "}";
    subtitleLabel->setStyleSheet(subtitleLabel->normalStyle);
    subtitleLabel->setMinimumHeight(32);  // 空内容时也保持最小高度
    subtitleLabel->hide();

    // 定位字幕标签：默认位置 + 用户拖动偏移量
    auto positionSubtitle = [&]() {
        if (!subtitleLabel->isVisible() || subtitleLabel->dragging) return;
        int w = subtitleLabel->width();
        int h = subtitleLabel->height();
        QPoint defaultPos = window.mapToGlobal(QPoint(
            (window.width() - w) / 2,
            window.height() - UI_TOTAL_HEIGHT - h - 10));
        subtitleLabel->move(defaultPos + subtitleLabel->offset);
    };

    // 拖动结束时更新offset（保持用户拖动的位置）
    subtitleLabel->onDragEnd = [&]() {
        int w = subtitleLabel->width();
        int h = subtitleLabel->height();
        QPoint defaultPos = window.mapToGlobal(QPoint(
            (window.width() - w) / 2,
            window.height() - UI_TOTAL_HEIGHT - h - 10));
        subtitleLabel->offset = subtitleLabel->pos() - defaultPos;
    };

    // 控制栏
    QWidget *controlBar = new QWidget(uiWindow);
    controlBar->setFixedHeight(CONTROL_HEIGHT);
    QHBoxLayout *controlLayout = new QHBoxLayout(controlBar);
    controlLayout->setContentsMargins(12, 0, 12, 0);

    QPushButton *openBtn = new QPushButton("打开", controlBar);
    openBtn->setFixedSize(64, 30);
    openBtn->setStyleSheet("QPushButton{color:white;background:#4a4a4a;border:none;border-radius:4px;}QPushButton:hover{background:#5a5a5a;}");

    QPushButton *playBtn = new QPushButton("播放", controlBar);
    playBtn->setFixedSize(64, 30);
    playBtn->setStyleSheet("QPushButton{color:white;background:#0078d4;border:none;border-radius:4px;}QPushButton:hover{background:#1a86e0;}");

    QSlider *progressSlider = new QSlider(Qt::Horizontal, controlBar);
    progressSlider->setRange(0, 1);
    progressSlider->setValue(0);

    QLabel *volumeIcon = new QLabel("🔊", controlBar);
    volumeIcon->setStyleSheet("color:#cccccc;font-size:14px;");
    volumeIcon->setFixedWidth(20);
    volumeIcon->setAlignment(Qt::AlignCenter);

    QSlider *volumeSlider = new QSlider(Qt::Horizontal, controlBar);
    volumeSlider->setRange(0, 100);
    volumeSlider->setValue(100);
    volumeSlider->setFixedWidth(80);

    QLabel *timeLabel = new QLabel("00:00 / 00:00", controlBar);
    timeLabel->setStyleSheet("color:#cccccc;font-size:12px;");
    timeLabel->setFixedWidth(110);
    timeLabel->setAlignment(Qt::AlignCenter);

    QPushButton *fullscreenBtn = new QPushButton("⛶", controlBar);
    fullscreenBtn->setFixedSize(40, 30);
    fullscreenBtn->setStyleSheet("QPushButton{color:white;background:#4a4a4a;border:none;border-radius:4px;font-size:16px;}QPushButton:hover{background:#5a5a5a;}");

    QPushButton *screenshotBtn = new QPushButton("📷", controlBar);
    screenshotBtn->setFixedSize(40, 30);
    screenshotBtn->setStyleSheet("QPushButton{color:white;background:#4a4a4a;border:none;border-radius:4px;font-size:16px;}QPushButton:hover{background:#5a5a5a;}");
    screenshotBtn->setToolTip("截图");

    controlLayout->addWidget(openBtn);
    controlLayout->addWidget(playBtn);
    controlLayout->addWidget(progressSlider, 1);
    controlLayout->addWidget(volumeIcon);
    controlLayout->addWidget(volumeSlider);
    controlLayout->addWidget(timeLabel);
    controlLayout->addWidget(screenshotBtn);
    controlLayout->addWidget(fullscreenBtn);

    // 播放列表面板（默认隐藏）
    QWidget *playlistPanel = new QWidget(uiWindow);
    playlistPanel->setFixedHeight(150);
    QVBoxLayout *plLayout = new QVBoxLayout(playlistPanel);
    plLayout->setContentsMargins(8, 4, 8, 4);
    plLayout->setSpacing(4);
    QListWidget *playlistWidget = new QListWidget(playlistPanel);
    playlistWidget->setStyleSheet(
        "QListWidget{background:#2a2a2a;color:#cccccc;border:1px solid #444;border-radius:3px;font-size:12px;}"
        "QListWidget::item{padding:4px 8px;}"
        "QListWidget::item:selected{background:#0078d4;color:white;}"
    );
    plLayout->addWidget(playlistWidget);
    QHBoxLayout *plBtnLayout = new QHBoxLayout();
    QPushButton *plAddBtn = new QPushButton("添加", playlistPanel);
    plAddBtn->setFixedHeight(24);
    plAddBtn->setStyleSheet("QPushButton{color:#cccccc;background:#3a3a3a;border:none;border-radius:3px;font-size:11px;}QPushButton:hover{background:#4a4a4a;}");
    QPushButton *plRemoveBtn = new QPushButton("移除", playlistPanel);
    plRemoveBtn->setFixedHeight(24);
    plRemoveBtn->setStyleSheet("QPushButton{color:#cccccc;background:#3a3a3a;border:none;border-radius:3px;font-size:11px;}QPushButton:hover{background:#4a4a4a;}");
    plBtnLayout->addWidget(plAddBtn);
    plBtnLayout->addWidget(plRemoveBtn);
    plBtnLayout->addStretch();
    plLayout->addLayout(plBtnLayout);
    playlistPanel->hide();

    uiLayout->addWidget(settingsBar);
    uiLayout->addWidget(playlistPanel);
    uiLayout->addWidget(controlBar);
    // 字幕标签不加入布局，悬浮在控制栏上方（避免被固定高度的UI窗口遮挡）

    // 定位UI窗口到主窗口底部
    auto positionUIWindow = [&]() {
        if (!uiWindow->isVisible()) return;
        int totalH = UI_TOTAL_HEIGHT + (playlistPanel->isVisible() ? playlistPanel->height() : 0);
        QPoint globalPos = window.mapToGlobal(QPoint(0, window.height() - totalH));
        uiWindow->setGeometry(globalPos.x(), globalPos.y(), window.width(), totalH);
        positionSubtitle();  // 同步更新字幕位置
    };

    PlayerController *player = new PlayerController(&window);

    // ===== Whisper AI 识别（T4.8 验证） =====
    WhisperTranscriber *whisper = new WhisperTranscriber(&window);
    bool aiRecording = false;
    bool rtActive = false;
    const QString whisperModelPath = AppPaths::resolveResource(QStringLiteral("ai/whisperModel"),
                                                               AppPaths::whisperModelCandidates());
    if (whisperModelPath.isEmpty()) {
        LOG_WARN(LogModule::AI_ASR, "whisper model not found, AI ASR disabled (set config key ai/whisperModel)");
    } else {
        // 后台加载，线程由 WhisperTranscriber 自己持有并在析构时 join
        QTimer::singleShot(500, whisper, [whisper, whisperModelPath]() {
            whisper->loadModelAsync(whisperModelPath);
        });
    }

    // ===== Translator AI 翻译（T4.8） =====
    Translator *translator = new Translator(&window);
    const QString translatorModelPath = AppPaths::resolveResource(QStringLiteral("ai/translatorModel"),
                                                                  AppPaths::translatorModelCandidates());
    if (translatorModelPath.isEmpty()) {
        LOG_WARN(LogModule::AI_MT, "translator model not found, AI translation disabled (set config key ai/translatorModel)");
    } else {
        // 延迟加载，让播放器先出首帧；线程登记在 Translator 内部，析构时 join
        QTimer::singleShot(2000, translator, [translator, translatorModelPath]() {
            translator->loadModelAsync(translatorModelPath);
        });
    }

    // ===== 实时字幕工作线程 =====
    RealtimeSubtitleWorker *rtWorker = new RealtimeSubtitleWorker();
    QThread *rtThread = new QThread(&window);
    rtWorker->moveToThread(rtThread);
    rtWorker->setSource(
        [player]() { return player->audioRingPosition(); },
        [player](qint64 absStart, float *out, int maxCount) { return player->copyAudioRingFrom(absStart, out, maxCount); });
    rtWorker->setEngines(whisper, translator);
    QObject::connect(rtThread, &QThread::started, rtWorker, &RealtimeSubtitleWorker::start);
    QObject::connect(rtThread, &QThread::finished, rtWorker, &QObject::deleteLater);
    QObject::connect(player, &PlayerController::seekStarted, rtWorker, &RealtimeSubtitleWorker::resetTimeline,
                     Qt::QueuedConnection);

    QObject::connect(translator, &Translator::modelLoaded, [&](bool success, const QString &error) {
        if (success) {
            qDebug() << "[Main] Translator model loaded";
        } else {
            qWarning() << "[Main] Translator model load failed:" << error;
        }
    });

    // ===== 设置持久化 =====
    QSettings settings("MediaPlayer", "Config");
    bool savedHwDecode = settings.value("hardwareDecode", true).toBool();
    int savedVolume = settings.value("volume", 100).toInt();

    hwCombo->setCurrentIndex(savedHwDecode ? 0 : 1);
    volumeSlider->setValue(savedVolume);

    player->setHardwareDecode(hwCombo->currentData().toBool());
    player->setVolume(volumeSlider->value() / 100.0f);

    // 拖拽打开文件：添加到播放列表并播放第一个
    auto handleDroppedFiles = [&](const QStringList &files) {
        for (const QString &f : files) {
            player->addToPlaylist(f);
        }
        if (!files.isEmpty()) {
            int idx = player->playlist().indexOf(files.first());
            if (idx >= 0) {
                player->playIndex(idx);
                playBtn->setText("暂停");
            }
        }
    };

    MainWindowFilter *mwFilter = new MainWindowFilter(positionUIWindow, handleDroppedFiles);
    window.installEventFilter(mwFilter);
    central->installEventFilter(mwFilter);
    central->setAcceptDrops(true);
    videoWidget->installEventFilter(mwFilter);
    videoWidget->setAcceptDrops(true);

    QObject::connect(hwCombo, &QComboBox::currentIndexChanged, [&]() {
        player->setHardwareDecode(hwCombo->currentData().toBool());
        settings.setValue("hardwareDecode", hwCombo->currentIndex() == 0);
    });
    QObject::connect(volumeSlider, &QSlider::valueChanged, [&](int value) {
        player->setVolume(value / 100.0f);
        volumeIcon->setText(value == 0 ? "🔇" : "🔊");
        settings.setValue("volume", value);
    });

    QObject::connect(player, &PlayerController::frameReady, videoWindow, &VideoWidget::setFrame);
    QObject::connect(player, &PlayerController::seekStarted, videoWindow, &VideoWidget::clearPending);
    QObject::connect(player, &PlayerController::errorOccurred, [&](const QString &err) {
        QMessageBox::warning(&window, "播放器错误", err);
    });

    qint64 totalDuration = 0;
    bool isSeeking = false;

    QObject::connect(player, &PlayerController::durationChanged, [&](qint64 ms) {
        totalDuration = ms;
        progressSlider->setRange(0, static_cast<int>(ms));
        int totalSec = static_cast<int>(ms / 1000);
        timeLabel->setText(QString("00:00 / %1:%2")
            .arg(totalSec / 60, 2, 10, QChar('0'))
            .arg(totalSec % 60, 2, 10, QChar('0')));
    });

    QObject::connect(player, &PlayerController::positionChanged, [&](qint64 ms) {
        if (!isSeeking) {
            progressSlider->setValue(static_cast<int>(ms));
        }
        int curSec = static_cast<int>(ms / 1000);
        int totalSec = static_cast<int>(totalDuration / 1000);
        timeLabel->setText(QString("%1:%2 / %3:%4")
            .arg(curSec / 60, 2, 10, QChar('0'))
            .arg(curSec % 60, 2, 10, QChar('0'))
            .arg(totalSec / 60, 2, 10, QChar('0'))
            .arg(totalSec % 60, 2, 10, QChar('0')));
    });

    QObject::connect(progressSlider, &QSlider::sliderPressed, [&]() {
        isSeeking = true;
    });
    QObject::connect(progressSlider, &QSlider::sliderReleased, [&]() {
        if (totalDuration > 0) {
            player->seek(static_cast<qint64>(progressSlider->value()));
        }
        isSeeking = false;
    });

    QObject::connect(openBtn, &QPushButton::clicked, [&]() {
        QString filePath = QFileDialog::getOpenFileName(&window, "打开视频文件", "",
            "视频文件 (*.mp4 *.mkv *.avi *.flv *.mov *.wmv *.webm);;所有文件 (*.*)");
        if (!filePath.isEmpty()) {
            player->addToPlaylist(filePath);
            int idx = player->playlist().indexOf(filePath);
            player->playIndex(idx);
            playBtn->setText("暂停");
        }
    });

    // 播放列表切换显示
    QObject::connect(playlistBtn, &QPushButton::toggled, [&](bool checked) {
        playlistPanel->setVisible(checked);
        playlistBtn->setStyleSheet(checked ?
            "QPushButton{color:white;background:#0078d4;border:none;border-radius:3px;font-size:11px;}QPushButton:hover{background:#1a86e0;}" :
            "QPushButton{color:#cccccc;background:#3a3a3a;border:none;border-radius:3px;font-size:11px;}QPushButton:hover{background:#4a4a4a;}");
        QTimer::singleShot(10, positionUIWindow);
    });

    // 播放列表：添加文件
    QObject::connect(plAddBtn, &QPushButton::clicked, [&]() {
        QStringList files = QFileDialog::getOpenFileNames(&window, "添加到播放列表", "",
            "视频文件 (*.mp4 *.mkv *.avi *.flv *.mov *.wmv *.webm);;所有文件 (*.*)");
        for (const QString &f : files) {
            player->addToPlaylist(f);
        }
    });

    // 播放列表：移除选中项
    QObject::connect(plRemoveBtn, &QPushButton::clicked, [&]() {
        int row = playlistWidget->currentRow();
        if (row >= 0) player->removeFromPlaylist(row);
    });

    // 播放列表：双击播放
    QObject::connect(playlistWidget, &QListWidget::itemDoubleClicked, [&](QListWidgetItem *item) {
        int row = playlistWidget->row(item);
        player->playIndex(row);
        playBtn->setText("暂停");
    });

    // 播放列表变化时更新列表
    QObject::connect(player, &PlayerController::playlistChanged, [&]() {
        playlistWidget->clear();
        for (const QString &f : player->playlist()) {
            playlistWidget->addItem(QFileInfo(f).fileName());
        }
        playlistWidget->setCurrentRow(player->currentIndex());
    });

    // 当前索引变化时高亮
    QObject::connect(player, &PlayerController::currentIndexChanged, [&](int index) {
        playlistWidget->setCurrentRow(index);
    });

    // 播放结束自动播放下一个
    QObject::connect(player, &PlayerController::playbackFinished, [&]() {
        if (player->currentIndex() + 1 < player->playlist().size()) {
            player->playNext();
        }
    });

    // 字幕开关：切换显示/隐藏
    QObject::connect(subtitleToggle, &QPushButton::toggled, [&](bool checked) {
        if (checked) {
            subtitleToggle->setStyleSheet("QPushButton{color:white;background:#0078d4;border:none;border-radius:3px;font-size:11px;}QPushButton:hover{background:#1a86e0;}");
            if (player->hasSubtitle()) {
                subtitleLabel->show();
            }
        } else {
            subtitleToggle->setStyleSheet("QPushButton{color:#cccccc;background:#3a3a3a;border:none;border-radius:3px;font-size:11px;}QPushButton:hover{background:#4a4a4a;}");
            subtitleLabel->hide();
        }
    });

    // 选择字幕文件
    QObject::connect(subtitleFileBtn, &QPushButton::clicked, [&]() {
        QString filePath = QFileDialog::getOpenFileName(&window, "加载字幕文件", "",
            "字幕文件 (*.srt *.ass *.ssa);;所有文件 (*.*)");
        if (!filePath.isEmpty()) {
            if (player->loadSubtitle(filePath)) {
                subtitleLabel->offset = QPoint(0, 0);
                subtitleLabel->setText("");
                subtitleLabel->adjustSize();
                positionSubtitle();
                subtitleToggle->setChecked(true);  // 加载后自动开启
            }
        }
    });

    // 字幕显示同步（加载后始终显示，空内容显示空条；开关关闭时隐藏）
    QObject::connect(player, &PlayerController::subtitleChanged, [&](const QString &text) {
        if (!player->hasSubtitle() || !subtitleToggle->isChecked()) {
            subtitleLabel->hide();
            return;
        }
        subtitleLabel->setText(text);
        int maxW = static_cast<int>(window.width() * 0.8);
        subtitleLabel->setMaximumWidth(maxW);
        subtitleLabel->adjustSize();
        positionSubtitle();
        subtitleLabel->show();
        subtitleLabel->raise();
    });

    QObject::connect(playBtn, &QPushButton::clicked, [&]() {
        if (player->isPlaying()) {
            player->pause();
            playBtn->setText("播放");
        } else {
            player->play();
            playBtn->setText("暂停");
        }
    });

    // ===== 全屏控制 =====
    QTimer *hideTimer = new QTimer(&window);
    hideTimer->setSingleShot(true);

    // UI隐藏延迟定时器（替代singleShot，可被showUI取消）
    QTimer *hideDelayTimer = new QTimer(&window);
    hideDelayTimer->setSingleShot(true);
    QObject::connect(hideDelayTimer, &QTimer::timeout, [&]() {
        if (uiWindow->windowOpacity() < 0.1) uiWindow->hide();
    });

    QPropertyAnimation *fadeAnim = new QPropertyAnimation(uiWindow, "windowOpacity");
    fadeAnim->setDuration(200);
    fadeAnim->setEasingCurve(QEasingCurve::InOutQuad);

    auto showUI = [&]() {
        hideDelayTimer->stop();  // 取消待执行的隐藏
        uiWindow->show();
        positionUIWindow();
        fadeAnim->stop();
        fadeAnim->setStartValue(uiWindow->windowOpacity());
        fadeAnim->setEndValue(1.0);
        fadeAnim->start();
    };
    auto hideUI = [&]() {
        fadeAnim->stop();
        fadeAnim->setStartValue(uiWindow->windowOpacity());
        fadeAnim->setEndValue(0.0);
        fadeAnim->start();
        hideDelayTimer->start(220);  // 渐变结束后隐藏，可被showUI取消
    };

    QObject::connect(hideTimer, &QTimer::timeout, [&]() {
        if (window.isFullScreen()) hideUI();
    });

    auto enterFullScreen = [&]() {
        uiWindow->hide();
        window.showFullScreen();
        QTimer::singleShot(200, [&]() {
            if (window.isFullScreen()) {
                positionUIWindow();
                uiWindow->show();
                hideTimer->start(2500);
            }
        });
    };
    auto exitFullScreen = [&]() {
        window.showNormal();
        showUI();
        hideTimer->stop();
    };
    auto toggleFullScreen = [&]() {
        if (window.isFullScreen()) exitFullScreen();
        else enterFullScreen();
    };

    QObject::connect(fullscreenBtn, &QPushButton::clicked, toggleFullScreen);

    // 淡入淡出通知（无需手动关闭）
    auto showNotification = [&](const QString &text) {
        QLabel *notify = new QLabel();
        notify->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
        notify->setAttribute(Qt::WA_TranslucentBackground);
        notify->setStyleSheet("background:rgba(40,40,40,230);color:white;padding:12px 28px;border-radius:6px;font-size:13px;");
        notify->setText(text);
        notify->adjustSize();
        QScreen *screen = QGuiApplication::primaryScreen();
        QRect geo = screen->availableGeometry();
        notify->move(geo.center().x() - notify->width()/2, geo.bottom() - 120);
        notify->setWindowOpacity(0);
        notify->show();
        QPropertyAnimation *fadeIn = new QPropertyAnimation(notify, "windowOpacity");
        fadeIn->setDuration(200);
        fadeIn->setStartValue(0.0);
        fadeIn->setEndValue(1.0);
        QPropertyAnimation *fadeOut = new QPropertyAnimation(notify, "windowOpacity");
        fadeOut->setDuration(300);
        fadeOut->setStartValue(1.0);
        fadeOut->setEndValue(0.0);
        QObject::connect(fadeIn, &QPropertyAnimation::finished, [fadeOut]() {
            QTimer::singleShot(2000, [fadeOut]() { fadeOut->start(); });
        });
        QObject::connect(fadeOut, &QPropertyAnimation::finished, notify, &QLabel::deleteLater);
        fadeIn->start();
    };

    // 翻译完成后更新字幕（T4.8）
    QObject::connect(translator, &Translator::batchTranslateFinished, &window, [&](const QVector<TranslationResult> &results) {
        QList<SubtitleCue> translatedCues = player->aiSubtitles();
        // 先清空所有旧译文，避免切换语言时新旧译文混合
        for (auto &cue : translatedCues) cue.translatedText = "";
        for (int i = 0; i < translatedCues.size() && i < results.size(); i++) {
            if (results[i].success && !results[i].translatedText.isEmpty()) {
                translatedCues[i].translatedText = results[i].translatedText;
            }
        }
        player->setAISubtitles(translatedCues);
        QTimer::singleShot(0, [&]() {
            showNotification("AI翻译完成");
        });
    }, Qt::QueuedConnection);

    // 切换目标语言时自动重新翻译已有AI字幕（T4.8）
    QObject::connect(translateLangCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &window, [&](int index) {
        Q_UNUSED(index);
        QString targetLang = translateLangCombo->currentData().toString();
        if (targetLang == "original") {
            // 切换到原文：清空翻译文本
            QList<SubtitleCue> cues = player->aiSubtitles();
            if (!cues.isEmpty()) {
                for (auto &cue : cues) cue.translatedText = "";
                player->setAISubtitles(cues);
            }
            return;
        }
        // 有AI字幕且模型已加载时自动重新翻译
        QList<SubtitleCue> cues = player->aiSubtitles();
        if (!cues.isEmpty() && translator->isModelLoaded()) {
            QVector<QString> texts;
            for (const auto &cue : cues) texts.append(cue.text);
            showNotification(QString("正在重新翻译为%1...").arg(targetLang));
            translator->translateBatchAsync(texts, targetLang);
        }
    });

    // Whisper 识别完成后设置为字幕（T4.8 验证）
    QObject::connect(whisper, &WhisperTranscriber::transcribeFinished, &window, [&](const QVector<TranscriptSegment> &segments) {
        QList<SubtitleCue> cues;
        for (const auto &seg : segments) {
            SubtitleCue cue;
            cue.startTime = seg.start;
            cue.endTime = seg.end;
            cue.text = seg.text;
            cues.append(cue);
        }
        player->setAISubtitles(cues);

        // 所有UI操作和翻译触发都派发到主线程
        QTimer::singleShot(0, [&, cues]() {
            QString targetLang = translateLangCombo->currentData().toString();
            bool needTranslate = (targetLang != "original" && translator->isModelLoaded());

            aiBtn->setText("AI识别");
            aiBtn->setEnabled(true);
            aiBtn->setStyleSheet("QPushButton{color:#cccccc;background:#3a3a3a;border:none;border-radius:3px;font-size:11px;}QPushButton:hover{background:#4a4a4a;}");
            if (needTranslate) {
                showNotification(QString("AI识别完成：%1条字幕，正在翻译为%2...").arg(cues.size()).arg(targetLang));
            } else {
                showNotification(QString("AI识别完成：%1条字幕").arg(cues.size()));
            }

            // 异步批量翻译
            if (needTranslate) {
                QVector<QString> texts;
                for (const auto &cue : cues) {
                    texts.append(cue.text);
                }
                translator->translateBatchAsync(texts, targetLang);
            }
        });
    }, Qt::QueuedConnection);

    QObject::connect(whisper, &WhisperTranscriber::errorOccurred, &window, [&](const QString &err) {
        QTimer::singleShot(0, [&, err]() {
            showNotification("AI识别错误：" + err);
            aiBtn->setText("AI识别");
            aiBtn->setEnabled(true);
            aiBtn->setStyleSheet("QPushButton{color:#cccccc;background:#3a3a3a;border:none;border-radius:3px;font-size:11px;}QPushButton:hover{background:#4a4a4a;}");
        });
    }, Qt::QueuedConnection);

    // 截图
    QObject::connect(screenshotBtn, &QPushButton::clicked, [&]() {
        QImage img = videoWindow->capture();
        if (img.isNull()) {
            showNotification("截图失败：无画面");
            return;
        }
        QString dir = QDir(QCoreApplication::applicationDirPath()).filePath("../../picture");
        QDir().mkpath(dir);
        QString filename = QString("screenshot_%1.png").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
        QString path = QDir(dir).filePath(filename);
        if (img.save(path, "PNG")) {
            showNotification(QString("截图已保存：%1").arg(filename));
        } else {
            showNotification("截图保存失败");
        }
    });

    // 实时字幕：文本直接进悬浮字幕条（按到达顺序显示，不参与 cue 时间轴）
    QObject::connect(rtWorker, &RealtimeSubtitleWorker::subtitleReady, &window,
                     [&](const QString &text, double t0, double t1, double procMs, double lagSec) {
        if (text.isEmpty()) return;
        subtitleLabel->setText(text);
        subtitleLabel->setMaximumWidth(static_cast<int>(window.width() * 0.8));
        subtitleLabel->adjustSize();
        positionSubtitle();
        subtitleLabel->show();
        subtitleLabel->raise();
        LOG_INFO(LogModule::AI_ASR, "realtime subtitle %.1f-%.1fs proc %.0fms lag %.1fs",
                 t0, t1, procMs, lagSec);
    }, Qt::QueuedConnection);

    QObject::connect(rtWorker, &RealtimeSubtitleWorker::statusChanged, &window, [&](const QString &status) {
        showNotification(status);
    }, Qt::QueuedConnection);

    QObject::connect(rtWorker, &RealtimeSubtitleWorker::errorOccurred, &window, [&](const QString &err) {
        showNotification("实时字幕：" + err);
        rtBtn->setText("实时字幕");
        rtBtn->setStyleSheet("QPushButton{color:#cccccc;background:#3a3a3a;border:none;border-radius:3px;font-size:11px;}QPushButton:hover{background:#4a4a4a;}");
    }, Qt::QueuedConnection);

    QObject::connect(rtBtn, &QPushButton::clicked, [&]() {
        if (!rtActive) {
            if (!whisper->isModelLoaded()) {
                showNotification("语音识别模型还在加载，请稍候");
                return;
            }
            const QString vadPath = AppPaths::resolveResource(QStringLiteral("ai/vadModel"),
                                                              AppPaths::vadModelCandidates());
            if (vadPath.isEmpty()) {
                showNotification("缺少 VAD 模型（third_party/whisper.cpp/models/ggml-silero-v5.1.2.bin）");
                return;
            }

            RealtimeSubtitleWorker::Config cfg;
            cfg.vadModelPath = vadPath;
            cfg.asrLanguage = "auto";
            const QString lang = translateLangCombo->currentData().toString();
            cfg.targetLang = (lang == "original" || !translator->isModelLoaded()) ? QString() : lang;
            rtWorker->configure(cfg);

            player->clearSubtitle();
            player->setAudioRingEnabled(true);
            rtThread->start();
            rtActive = true;
            rtBtn->setText("停止实时");
            rtBtn->setStyleSheet("QPushButton{color:white;background:#d4380d;border:none;border-radius:3px;font-size:11px;}QPushButton:hover{background:#e8542a;}");
        } else {
            rtActive = false;
            rtWorker->requestStop();   // 线程安全：正在识别也能尽快退出
            rtThread->quit();
            rtThread->wait();          // 最多等一段识别的时长
            player->setAudioRingEnabled(false);
            rtBtn->setText("实时字幕");
            rtBtn->setStyleSheet("QPushButton{color:#cccccc;background:#3a3a3a;border:none;border-radius:3px;font-size:11px;}QPushButton:hover{background:#4a4a4a;}");
            subtitleLabel->hide();
        }
    });

    // AI识别按钮（T4.8验证）
    QObject::connect(aiBtn, &QPushButton::clicked, [&]() {
        if (!whisper->isModelLoaded()) {
            showNotification("AI模型加载中，请稍候...");
            return;
        }
        if (!aiRecording) {
            // 开始录制
            player->startAudioTap();
            player->clearSubtitle();
            aiRecording = true;
            aiBtn->setText("停止识别");
            aiBtn->setStyleSheet("QPushButton{color:white;background:#d4380d;border:none;border-radius:3px;font-size:11px;}QPushButton:hover{background:#e8542a;}");
            showNotification("AI识别录制中...播放视频后点击停止识别");
        } else {
            // 停止录制并识别
            aiRecording = false;
            player->stopAudioTap();
            QVector<float> samples = player->getAudioTapSamples();
            if (samples.isEmpty()) {
                showNotification("没有录制到音频");
                aiBtn->setText("AI识别");
                aiBtn->setStyleSheet("QPushButton{color:#cccccc;background:#3a3a3a;border:none;border-radius:3px;font-size:11px;}QPushButton:hover{background:#4a4a4a;}");
                return;
            }
            aiBtn->setText("识别中...");
            aiBtn->setEnabled(false);
            showNotification(QString("正在识别 %1 秒音频...").arg((float)samples.size() / 16000.0f, 0, 'f', 1));
            // 识别在 WhisperTranscriber 内部的工作线程执行，对象析构时会 join 该线程
            whisper->transcribeAsync(samples, "auto");
        }
    });

    // 关于对话框
    QObject::connect(aboutBtn, &QPushButton::clicked, [&]() {
        QDialog aboutDlg(&window);
        aboutDlg.setWindowTitle("关于 MediaPlayer");
        aboutDlg.setFixedSize(520, 480);
        aboutDlg.setStyleSheet("QDialog{background:#2b2b2b;}QLabel{color:#dddddd;}QTextEdit{color:#cccccc;background:#1e1e1e;border:1px solid #444;border-radius:4px;}QPushButton{color:white;background:#0078d4;border:none;border-radius:4px;padding:6px 20px;}QPushButton:hover{background:#1a86e0;}");

        QVBoxLayout *layout = new QVBoxLayout(&aboutDlg);
        layout->setContentsMargins(20, 20, 20, 20);
        layout->setSpacing(10);

        QLabel *titleLabel = new QLabel("MediaPlayer", &aboutDlg);
        titleLabel->setStyleSheet("font-size:22px;font-weight:bold;color:white;");
        titleLabel->setAlignment(Qt::AlignCenter);
        layout->addWidget(titleLabel);

        QLabel *verLabel = new QLabel(QString("版本 %1  |  基于 Qt6 + FFmpeg + SDL2 + D3D11").arg(MP_APP_VERSION), &aboutDlg);
        verLabel->setStyleSheet("font-size:11px;color:#888888;");
        verLabel->setAlignment(Qt::AlignCenter);
        layout->addWidget(verLabel);

        QLabel *descLabel = new QLabel("一款支持无损播放、硬件解码、离线AI翻译字幕的桌面视频播放器。", &aboutDlg);
        descLabel->setStyleSheet("font-size:12px;color:#aaaaaa;");
        descLabel->setAlignment(Qt::AlignCenter);
        descLabel->setWordWrap(true);
        layout->addWidget(descLabel);

        QLabel *ossTitle = new QLabel("开源组件", &aboutDlg);
        ossTitle->setStyleSheet("font-size:13px;font-weight:bold;color:#0078d4;");
        layout->addWidget(ossTitle);

        QTextEdit *ossText = new QTextEdit(&aboutDlg);
        ossText->setReadOnly(true);
        ossText->setFixedHeight(140);
        ossText->setHtml(
            "<table style='font-size:11px;'>"
            "<tr><td style='padding:2px 8px 2px 0;'><b>Qt 6</b></td><td style='color:#888;'>LGPL-3.0</td><td style='color:#666;padding-left:12px;'>跨平台应用框架</td></tr>"
            "<tr><td style='padding:2px 8px 2px 0;'><b>FFmpeg</b></td><td style='color:#888;'>LGPL-2.1+</td><td style='color:#666;padding-left:12px;'>音视频解码（LGPL构建）</td></tr>"
            "<tr><td style='padding:2px 8px 2px 0;'><b>SDL2</b></td><td style='color:#888;'>zlib</td><td style='color:#666;padding-left:12px;'>音频输出</td></tr>"
            "<tr><td style='padding:2px 8px 2px 0;'><b>whisper.cpp</b></td><td style='color:#888;'>MIT</td><td style='color:#666;padding-left:12px;'>离线语音识别（T4.8）</td></tr>"
            "<tr><td style='padding:2px 8px 2px 0;'><b>llama.cpp</b></td><td style='color:#888;'>MIT</td><td style='color:#666;padding-left:12px;'>离线翻译推理（T4.8）</td></tr>"
            "<tr><td style='padding:2px 8px 2px 0;'><b>Index-Translate</b></td><td style='color:#888;'>Apache-2.0</td><td style='color:#666;padding-left:12px;'>翻译模型（T4.8）</td></tr>"
            "<tr><td style='padding:2px 8px 2px 0;'><b>Silero VAD</b></td><td style='color:#888;'>MIT</td><td style='color:#666;padding-left:12px;'>语音活动检测（T4.8）</td></tr>"
            "</table>"
        );
        layout->addWidget(ossText);

        QLabel *eulaTitle = new QLabel("免责声明", &aboutDlg);
        eulaTitle->setStyleSheet("font-size:13px;font-weight:bold;color:#0078d4;");
        layout->addWidget(eulaTitle);

        QTextEdit *eulaText = new QTextEdit(&aboutDlg);
        eulaText->setReadOnly(true);
        eulaText->setFixedHeight(100);
        eulaText->setStyleSheet("font-size:10px;");
        eulaText->setPlainText(
            "本软件按\"现状\"提供，不提供任何明示或暗示的保证，包括但不限于适销性、特定用途适用性和非侵权性的保证。\n"
            "使用本软件产生的一切风险由用户自行承担。开发者不对因使用本软件导致的任何直接或间接损失负责。\n"
            "AI翻译字幕功能仅供参考，翻译结果可能存在错误，不应用于医疗、法律、金融等专业场景。\n"
            "本软件仅提供播放工具，用户对所播放内容的合法性自行负责。"
        );
        layout->addWidget(eulaText);

        QPushButton *okBtn = new QPushButton("确定", &aboutDlg);
        okBtn->setFixedWidth(100);
        QObject::connect(okBtn, &QPushButton::clicked, &aboutDlg, &QDialog::accept);
        layout->addWidget(okBtn, 0, Qt::AlignCenter);

        aboutDlg.exec();
    });

    // ===== T4.9 诊断面板 =====
    DiagnosticPanel *diagPanel = new DiagnosticPanel(player, &window);
    QObject::connect(diagPanel, &DiagnosticPanel::exportRequested, [&]() {
        QString path = DiagnosticExporter::exportDiagnostic(player);
        showNotification(QString("诊断包已导出：%1").arg(QFileInfo(path).fileName()));
        LOG_INFO(LogModule::CORE, "Diagnostic exported to: %s", path.toUtf8().constData());
    });

    // Ctrl+D：显示/隐藏诊断面板
    QShortcut *ctrlDShortcut = new QShortcut(QKeySequence("Ctrl+D"), &window);
    QObject::connect(ctrlDShortcut, &QShortcut::activated, [&]() {
        if (diagPanel->isVisible()) diagPanel->hide();
        else { diagPanel->refresh(); diagPanel->show(); }
    });
    // Ctrl+E：一键导出诊断包
    QShortcut *ctrlEShortcut = new QShortcut(QKeySequence("Ctrl+E"), &window);
    QObject::connect(ctrlEShortcut, &QShortcut::activated, [&]() {
        QString path = DiagnosticExporter::exportDiagnostic(player);
        showNotification(QString("诊断包已导出：%1").arg(QFileInfo(path).fileName()));
    });

    QShortcut *escShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), &window);
    QObject::connect(escShortcut, &QShortcut::activated, [&]() {
        if (window.isFullScreen()) exitFullScreen();
    });
    QShortcut *fShortcut = new QShortcut(QKeySequence(Qt::Key_F), &window);
    QObject::connect(fShortcut, &QShortcut::activated, toggleFullScreen);

    // 空格：播放/暂停
    QShortcut *spaceShortcut = new QShortcut(QKeySequence(Qt::Key_Space), &window);
    QObject::connect(spaceShortcut, &QShortcut::activated, [&]() {
        if (player->isPlaying()) { player->pause(); playBtn->setText("播放"); }
        else { player->play(); playBtn->setText("暂停"); }
    });
    // ←/→：快退/快进5秒
    QShortcut *leftShortcut = new QShortcut(QKeySequence(Qt::Key_Left), &window);
    QObject::connect(leftShortcut, &QShortcut::activated, [&]() {
        if (totalDuration > 0) player->seek(qMax(0LL, static_cast<qint64>(progressSlider->value()) - 5000));
    });
    QShortcut *rightShortcut = new QShortcut(QKeySequence(Qt::Key_Right), &window);
    QObject::connect(rightShortcut, &QShortcut::activated, [&]() {
        if (totalDuration > 0) player->seek(qMin(totalDuration, static_cast<qint64>(progressSlider->value()) + 5000));
    });
    // ↑/↓：音量±5
    QShortcut *upShortcut = new QShortcut(QKeySequence(Qt::Key_Up), &window);
    QObject::connect(upShortcut, &QShortcut::activated, [&]() { volumeSlider->setValue(qMin(100, volumeSlider->value() + 5)); });
    QShortcut *downShortcut = new QShortcut(QKeySequence(Qt::Key_Down), &window);
    QObject::connect(downShortcut, &QShortcut::activated, [&]() { volumeSlider->setValue(qMax(0, volumeSlider->value() - 5)); });

    // 全屏时鼠标移动显示UI
    QTimer *mouseTimer = new QTimer(&window);
    QPoint lastMousePos = QCursor::pos();
    QObject::connect(mouseTimer, &QTimer::timeout, [&]() {
        if (!window.isFullScreen()) return;
        QPoint pos = QCursor::pos();
        if (pos != lastMousePos) {
            lastMousePos = pos;
            showUI();
            hideTimer->start(2500);
        }
    });
    mouseTimer->start(200);

    window.show();
    uiWindow->show();
    QTimer::singleShot(50, positionUIWindow);

    // 延迟安装 CrashHandler：等待D3D11初始化完成，避免捕获驱动层预期异常
    QTimer::singleShot(1000, []() {
        CrashHandler::install();
        LOG_INFO(LogModule::CRASH, "Crash handler installed after D3D11 init");
    });

    return app.exec();
}
