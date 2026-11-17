// 录音详情：高分辨率波形 + 整段声谱图 + 拖动选区播放。
#pragma once

#include "core/recordingstore.h"
#include "core/settings.h"

#include <QDialog>

class QAudioOutput;
class QLabel;
class QMediaPlayer;
class QPushButton;
class DetailWaveform;
class StaticSpectrogram;

class RecordingDetailDialog : public QDialog
{
    Q_OBJECT

public:
    explicit RecordingDetailDialog(const Recording &recording, const Settings &settings,
                                   QWidget *parent = nullptr);
    ~RecordingDetailDialog() override;

    void selectRangeForTest(qint64 startMs, qint64 endMs);
    void zoomForTest(double factor);
    void wheelForTest(int steps);

private:
    void loadAudio();
    void applyView(qint64 start, qint64 end);
    void updateViewUi();
    void togglePlayback();
    void playSelection();
    void clearSelection();
    void updatePlayButton();
    void updateTime(qint64 position, qint64 duration);
    void setStatus(const QString &text);

    Recording m_recording;
    Settings m_settings;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audio = nullptr;
    DetailWaveform *m_wave = nullptr;
    StaticSpectrogram *m_spectrogram = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_meta = nullptr;
    QLabel *m_time = nullptr;
    QLabel *m_selectionText = nullptr;
    QLabel *m_zoomText = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_play = nullptr;
    QPushButton *m_playSelection = nullptr;
    QPushButton *m_clearSelection = nullptr;
    QPushButton *m_fit = nullptr;

    qint64 m_selectionStart = -1;
    qint64 m_selectionEnd = -1;
    qint64 m_duration = 0;
    qint64 m_viewStart = 0;
    qint64 m_viewEnd = 0;
    bool m_selectionPlayback = false;
};
