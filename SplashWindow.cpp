#include "SplashWindow.h"

#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QPropertyAnimation>
#include <QtMath>

#include "Theme.h"
#include "Animation.h"

SplashWindow::SplashWindow(QWidget *parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool) {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);
    setFixedSize(520, 300);

    // 波浪相位推进，只重绘不触发布局
    m_waveTimer = new QTimer(this);
    m_waveTimer->setInterval(Anim::WaveRefreshMs);
    connect(m_waveTimer, &QTimer::timeout, this, [this] {
        m_phase += 0.08;
        update();
    });
    m_waveTimer->start();

    m_fade = new QPropertyAnimation(this, "windowOpacity", this);
    m_fade->setDuration(Anim::SplashFadeMs);
    m_fade->setStartValue(1.0);
    m_fade->setEndValue(0.0);
    connect(m_fade, &QPropertyAnimation::finished, this, [this] {
        emit finished();
        close();
    });
}

SplashWindow::~SplashWindow() = default;

void SplashWindow::setProgress(double frac) {
    m_progress = qBound(0.0, frac, 1.0);
    update();
}

void SplashWindow::setStage(const QString &label) {
    m_label = label;
    update();
}

void SplashWindow::finishAndClose() {
    // 只淡出一次
    if (m_fade->state() == QAbstractAnimation::Running) {
        return;
    }
    m_waveTimer->stop();
    m_fade->start();
}

void SplashWindow::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // 圆角背景
    const QRectF card(20, 20, width() - 40, height() - 40);
    p.setPen(Qt::NoPen);
    p.setBrush(Theme::Dark::SurfaceAlt);
    p.drawRoundedRect(card, Theme::RadiusOverlay, Theme::RadiusOverlay);

    // Logo 占位：圆 + "MP"（正式美术图到位后替换）
    const QPointF logoCenter(card.center().x(), card.top() + 84);
    p.setBrush(Theme::Dark::Accent);
    p.drawEllipse(logoCenter, 34, 34);
    p.setPen(Theme::Dark::TextPrimary);
    QFont logoFont = p.font();
    logoFont.setPixelSize(Theme::FontTitle);
    logoFont.setBold(true);
    p.setFont(logoFont);
    p.drawText(QRectF(logoCenter.x() - 34, logoCenter.y() - 34, 68, 68),
               Qt::AlignCenter, QStringLiteral("MP"));

    // 阶段文字
    p.setPen(QColor(Theme::Dark::TextPrimary.red(),
                    Theme::Dark::TextPrimary.green(),
                    Theme::Dark::TextPrimary.blue(),
                    int(255 * Theme::OpacityPrimary)));
    QFont stageFont = p.font();
    stageFont.setPixelSize(Theme::FontEmph);
    p.setFont(stageFont);
    p.drawText(QRectF(card.left(), card.top() + 148, card.width(), 28),
               Qt::AlignHCenter | Qt::AlignVCenter, m_label);

    // 波浪进度条轨道
    const QRectF track(card.left() + 56, card.bottom() - 62, card.width() - 112, 26);
    p.setBrush(Theme::Dark::Surface);
    p.setPen(Theme::Dark::Border);
    p.drawRoundedRect(track, Theme::RadiusControl, Theme::RadiusControl);

    // 填充区（按进度裁剪）
    if (m_progress > 0.0) {
        QPainterPath clip;
        const double fillW = track.width() * m_progress;
        clip.addRoundedRect(QRectF(track.left(), track.top(), fillW, track.height()),
                            Theme::RadiusControl, Theme::RadiusControl);
        p.save();
        p.setClipPath(clip);

        // 两条正弦波叠加，让"加载中"看起来是活的
        const int midY = int(track.center().y());
        QPainterPath wave;
        wave.moveTo(track.left() - 40, midY);
        for (int x = -40; x <= int(track.width()) + 40; ++x) {
            const double y1 = midY + 7.0 * qSin((x + m_phase * 40.0) / 26.0);
            const double y2 = midY + 4.0 * qCos((x + m_phase * 55.0) / 18.0);
            wave.lineTo(track.left() + x, (y1 + y2) * 0.5);
        }
        p.setPen(QPen(Theme::Dark::Accent, 3.0, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawPath(wave);
        p.restore();
    }

    // 百分比
    p.setPen(Theme::Dark::TextTertiary);
    QFont pctFont = p.font();
    pctFont.setPixelSize(Theme::FontAux);
    p.setFont(pctFont);
    p.drawText(QRectF(card.left(), card.bottom() - 30, card.width(), 18),
               Qt::AlignHCenter | Qt::AlignVCenter,
               QStringLiteral("%1%").arg(int(m_progress * 100)));
}
