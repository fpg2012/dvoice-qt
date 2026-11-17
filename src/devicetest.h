// 麦克风 / 扬声器测试对话框。
#pragma once

#include "core/player.h"
#include "core/recorder.h"
#include "core/settings.h"

#include <QDialog>

class QAudioSink;
class QBuffer;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSlider;
class QTimer;
class AudioMonitor;

class MicTestDialog : public QDialog
{
    Q_OBJECT

public:
    explicit MicTestDialog(const Settings &settings, QWidget *parent = nullptr);
    ~MicTestDialog() override;

private:
    void restart();

    Settings m_settings;
    Recorder m_recorder;
    AudioMonitor *m_monitor = nullptr;
    QComboBox *m_device = nullptr;
    QProgressBar *m_level = nullptr;
    QLabel *m_levelText = nullptr;
    QLabel *m_status = nullptr;
};

class SpeakerTestDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SpeakerTestDialog(const Settings &settings, QWidget *parent = nullptr);
    ~SpeakerTestDialog() override;

private:
    void refreshDevices();
    void applyVolume();
    void startTone();
    void startSpeech();
    void stopAll();
    void cleanupTone();
    void setStatus(const QString &text);

    Settings m_settings;
    Player m_player;
    AudioMonitor *m_monitor = nullptr;
    QComboBox *m_device = nullptr;
    QSlider *m_volume = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_stop = nullptr;

    QAudioSink *m_toneSink = nullptr;
    QBuffer *m_toneBuffer = nullptr;
    QByteArray m_tone;
};
