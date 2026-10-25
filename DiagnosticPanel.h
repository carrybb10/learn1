#ifndef DIAGNOSTICPANEL_H
#define DIAGNOSTICPANEL_H

#include <QDialog>
#include <QTimer>
#include <QTextEdit>
#include <QLabel>
#include <QComboBox>
#include <QPushButton>

class PlayerController;

class DiagnosticPanel : public QDialog {
    Q_OBJECT
public:
    explicit DiagnosticPanel(PlayerController* player, QWidget* parent = nullptr);
    void refresh();

signals:
    void exportRequested();

private slots:
    void onTimer();
    void onLogLevelChanged(int index);
    void onExportClicked();

private:
    void setupUI();
    void updatePlaybackInfo();
    void updateDecodeInfo();
    void updateD3D11Info();
    void updateAIInfo();
    void updateSystemInfo();
    void updateLogView();

    PlayerController* m_player;
    QTimer* m_timer;

    // 状态标签
    QLabel* m_fileLabel;
    QLabel* m_resolutionLabel;
    QLabel* m_positionLabel;
    QLabel* m_clockDiffLabel;
    QLabel* m_fpsLabel;
    QLabel* m_queueLabel;
    QLabel* m_d3d11Label;
    QLabel* m_aiLabel;
    QLabel* m_systemLabel;

    // 日志视图
    QTextEdit* m_logView;
    QComboBox* m_logLevelCombo;
    QPushButton* m_exportBtn;
    QPushButton* m_clearBtn;

    QString m_logFilter;
    int m_lastLogCount;
};

#endif // DIAGNOSTICPANEL_H
