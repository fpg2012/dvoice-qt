// 麦克风采集：固定 16 kHz / 单声道 / 16-bit，内置静音断句（VAD）。
#pragma once

#include <QAudioDevice>
#include <QAudioFormat>
#include <QByteArray>
#include <QObject>

#include <deque>

class QAudioSource;
class QIODevice;

struct VadConfig {
    int silenceMs = 650;
    int minSpeechMs = 300;
    int maxSegmentMs = 15000;
};

class Recorder : public QObject
{
    Q_OBJECT

public:
    explicit Recorder(QObject *parent = nullptr);
    ~Recorder() override;

    // ASR 上传格式：16 kHz、单声道、16 位小端。
    static QAudioFormat format();

    bool start(const QAudioDevice &device, const VadConfig &config, QString *error);
    void stop();
    void abort();

    // 调试/测试：不打开音频设备，直接把 16 kHz 单声道 PCM 喂给分段逻辑。
    void beginTestFeed(const VadConfig &config);
    void feedTestPcm(const QByteArray &pcm);
    void endTestFeed();
    bool isRunning() const { return m_source != nullptr; }
    QAudioDevice device() const { return m_device; }

signals:
    void pcmChunk(const QByteArray &pcm);
    void level(qreal rms, qreal peak);
    void segment(const QByteArray &pcm, int startMs, int endMs);
    void speechChanged(bool speaking);
    void errorOccurred(const QString &message);

private:
    void handleReadyRead();
    void processFrame(const char *data);
    void beginSpeech();
    void finishSegment(bool force);
    void resetState();

    QAudioSource *m_source = nullptr;
    QIODevice *m_io = nullptr;
    QAudioDevice m_device;
    VadConfig m_config;

    QByteArray m_pending;
    QByteArray m_segment;
    std::deque<QByteArray> m_preroll;
    std::deque<int> m_prerollMs;

    qint64 m_totalSamples = 0;
    int m_elapsedMs = 0;
    int m_speechStartMs = 0;
    int m_silenceFrames = 0;
    int m_voiceFrames = 0;
    qreal m_noise = 0.0015;
    qreal m_threshold = 0.006;
    bool m_speaking = false;
    bool m_stopping = false;
};
