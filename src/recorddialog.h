// 录音对话框：可配置采样率/声道/位深，实时电平，停止后自动存入录音库。
#pragma once

#include "core/audiorecorder.h"
#include "core/settings.h"

#include <QDialog>

class AudioMonitor;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QTimer;
class RecordingStore;

class RecordDialog : public QDialog
{
    Q_OBJECT

public:
    explicit RecordDialog(RecordingStore *store, const Settings &settings,
                          QWidget *parent = nullptr);
    ~RecordDialog() override;

signals:
    void saved(const QString &id);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void startRecording();
    void stopRecording();
    void saveRecording();
    void updateElapsed();
    void applyFormat();
    void setRecordingUi(bool recording);

    RecordingStore *m_store;
    Settings m_settings;
    AudioRecorder m_recorder;
    AudioMonitor *m_monitor = nullptr;
    QComboBox *m_device = nullptr;
    QComboBox *m_rate = nullptr;
    QComboBox *m_channels = nullptr;
    QComboBox *m_bits = nullptr;
    QProgressBar *m_level = nullptr;
    QLabel *m_levelText = nullptr;
    QLabel *m_elapsed = nullptr;
    QLabel *m_size = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_recordButton = nullptr;
    QTimer *m_timer = nullptr;
};
