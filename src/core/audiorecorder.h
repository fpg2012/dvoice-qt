// 录音采集：可配置采样率/声道/位深，PCM 保存在内存中，导出 WAV。
#pragma once

#include <QAudioDevice>
#include <QAudioFormat>
#include <QByteArray>
#include <QObject>
#include <QVector>

class QAudioSource;
class QIODevice;

class AudioRecorder : public QObject
{
    Q_OBJECT

public:
    explicit AudioRecorder(QObject *parent = nullptr);
    ~AudioRecorder() override;

    static QAudioFormat formatFor(int sampleRate, int channels, int bits);

    bool start(const QAudioDevice &device, int sampleRate, int channels, int bits,
               QString *error);
    void stop();
    void clear();

    bool isRunning() const { return m_source != nullptr; }
    int bits() const { return m_bits; }
    const QAudioFormat &format() const { return m_format; }
    const QByteArray &data() const { return m_data; }
    qint64 durationMs() const;
    qint64 byteSize() const { return m_data.size(); }

    QByteArray wavData(int outputBits) const;
    QByteArray pcm16Mono() const;
    QVector<float> peaks(int count) const;

signals:
    void level(qreal rms, qreal peak);
    void monitorChunk(const QByteArray &mono16, int sampleRate);
    void durationChanged(qint64 ms);
    void limitReached();
    void stopped();
    void errorOccurred(const QString &message);

private:
    void handleReadyRead();
    void release();

    QAudioSource *m_source = nullptr;
    QIODevice *m_io = nullptr;
    QAudioFormat m_format;
    QByteArray m_data;
    int m_bits = 16;
    qint64 m_lastDurationMs = 0;
};
