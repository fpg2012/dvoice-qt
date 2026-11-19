// 录音采集：可配置采样率/声道/位深，PCM 保存在内存中，导出 WAV。
#pragma once

#include <QAudioDevice>
#include <QAudioFormat>
#include <QByteArray>
#include <QObject>
#include <QString>
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
               qreal gain, QString *error);
    void setGain(qreal gain) { m_gain = qBound(0.0, gain, 20.0); }
    qreal gain() const { return m_gain; }
    void stop();
    void clear();

    bool isRunning() const { return m_source != nullptr; }
    int bits() const { return m_bits; }
    const QAudioFormat &format() const { return m_format; }
    // 设备实际格式与请求不一致时为 true，note() 给出可读说明。
    bool formatAdjusted() const { return m_adjusted; }
    const QString &formatNote() const { return m_note; }
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
    QByteArray applyGain(const QByteArray &data) const;

    QAudioSource *m_source = nullptr;
    QIODevice *m_io = nullptr;
    QAudioFormat m_format;
    QByteArray m_data;
    int m_bits = 16;
    bool m_adjusted = false;
    QString m_note;
    qreal m_gain = 1.0;
    qint64 m_lastDurationMs = 0;
};
