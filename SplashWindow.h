#pragma once
// Layer 0：启动闪屏。Logo 占位 + 波浪进度条 + 阶段文字。
#include <QWidget>
#include <QString>

class QTimer;
class QPropertyAnimation;

class SplashWindow : public QWidget {
    Q_OBJECT
public:
    explicit SplashWindow(QWidget *parent = nullptr);
    ~SplashWindow() override;

    // 0..1；波浪进度条的填充比例
    void setProgress(double frac);
    // 阶段说明（如"正在加载语音识别模型"）
    void setStage(const QString &label);
    // 淡出后自动关闭
    void finishAndClose();

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    QString m_label;
    double m_progress = 0.0;
    double m_phase = 0.0;  // 波浪相位，定时器推进
    QTimer *m_waveTimer = nullptr;
    QPropertyAnimation *m_fade = nullptr;
};
