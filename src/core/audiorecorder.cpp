#include "core/audiorecorder.h"

#include "core/pcm.h"
#include "core/wav.h"

#include <QAudioSource>
#include <QIODevice>
#include <QtEndian>

#include <cmath>

namespace {
constexpr qint64 kMaxBytes = 512LL * 1024 * 1024;
}

AudioRecorder::AudioRecorder(QObject *parent)
    : QObject(parent)
{
}

AudioRecorder::~AudioRecorder()
{
    release();
}

QAudioFormat AudioRecorder::formatFor(int sampleRate, int channels, int bits)
{
    QAudioFormat format;
    format.setSampleRate(sampleRate);
    format.setChannelCount(channels);
    format.setSampleFormat(bits <= 16 ? QAudioFormat::Int16 : QAudioFormat::Int32);
    return format;
}

bool AudioRecorder::start(const QAudioDevice &device, int sampleRate, int channels, int bits,
                          qreal gain, QString *error)
{
    release();
    m_data.clear();
    m_lastDurationMs = 0;

    if (device.isNull()) {
        if (error)
            *error = QStringLiteral("没有可用的输入设备");
        return false;
    }

    m_bits = (bits == 16 || bits == 24 || bits == 32) ? bits : 16;
    m_gain = qBound(0.0, gain, 20.0);
    m_format = formatFor(sampleRate, channels, m_bits);
    if (!device.isFormatSupported(m_format)) {
        if (error) {
            *error = QStringLiteral("「%1」不支持 %2 Hz / %3 声道 / %4 位录音")
                         .arg(device.description())
                         .arg(sampleRate)
                         .arg(channels)
                         .arg(m_bits);
        }
        return false;
    }

    m_source = new QAudioSource(device, m_format, this);
    m_source->setBufferSize(m_format.bytesForDuration(100000));
    m_io = m_source->start();
    if (!m_io || m_source->error() != QAudio::NoError) {
        if (error)
            *error = QStringLiteral("无法打开麦克风：%1").arg(device.description());
        release();
        return false;
    }
    connect(m_io, &QIODevice::readyRead, this, &AudioRecorder::handleReadyRead);
    return true;
}

void AudioRecorder::stop()
{
    if (!m_source)
        return;
    if (m_io)
        handleReadyRead();
    release();
    emit stopped();
}

void AudioRecorder::clear()
{
    m_data.clear();
    m_lastDurationMs = 0;
}

void AudioRecorder::release()
{
    if (m_source) {
        m_source->disconnect(this);
        m_source->stop();
        m_source->deleteLater();
        m_source = nullptr;
    }
    if (m_io) {
        m_io->disconnect(this);
        m_io = nullptr;
    }
}

qint64 AudioRecorder::durationMs() const
{
    const int frameBytes = m_format.bytesPerFrame();
    if (frameBytes <= 0 || m_format.sampleRate() <= 0)
        return 0;
    const qint64 frames = m_data.size() / frameBytes;
    return frames * 1000 / m_format.sampleRate();
}

QByteArray AudioRecorder::applyGain(const QByteArray &data) const
{
    if (data.isEmpty() || qFuzzyCompare(m_gain, 1.0))
        return data;

    QByteArray out = data;
    char *bytes = out.data();
    if (m_format.sampleFormat() == QAudioFormat::Int16) {
        const int count = out.size() / 2;
        for (int i = 0; i < count; ++i) {
            const qint16 value = qFromLittleEndian<qint16>(bytes + i * 2);
            const qint64 boosted = std::llround(qint64(value) * m_gain);
            const qint16 clamped = qint16(qBound<qint64>(-32768LL, boosted, 32767LL));
            qToLittleEndian(clamped, bytes + i * 2);
        }
    } else if (m_format.sampleFormat() == QAudioFormat::Int32) {
        const int count = out.size() / 4;
        for (int i = 0; i < count; ++i) {
            const qint32 value = qFromLittleEndian<qint32>(bytes + i * 4);
            const qint64 boosted = std::llround(double(value) * m_gain);
            const qint32 clamped = qint32(
                qBound<qint64>(-2147483648LL, boosted, 2147483647LL));
            qToLittleEndian(clamped, bytes + i * 4);
        }
    }
    return out;
}

void AudioRecorder::handleReadyRead()
{
    if (!m_io)
        return;
    const QByteArray chunk = applyGain(m_io->readAll());
    if (chunk.isEmpty())
        return;
    m_data.append(chunk);

    const QByteArray mono = pcm::mono16(chunk, m_format, m_format.sampleRate());
    if (!mono.isEmpty()) {
        emit monitorChunk(mono, m_format.sampleRate());

        qint64 sum = 0;
        int peak = 0;
        const int count = mono.size() / 2;
        for (int i = 0; i < count; ++i) {
            const qint16 sample = qFromLittleEndian<qint16>(mono.constData() + i * 2);
            sum += qint64(sample) * sample;
            peak = qMax(peak, qAbs(int(sample)));
        }
        if (count > 0) {
            const qreal rms = std::sqrt(qreal(sum) / count) / 32768.0;
            emit level(rms, qreal(peak) / 32768.0);
        }
    }

    const qint64 duration = durationMs();
    if (duration != m_lastDurationMs) {
        m_lastDurationMs = duration;
        emit durationChanged(duration);
    }

    if (m_data.size() >= kMaxBytes) {
        emit limitReached();
        stop();
    }
}

QByteArray AudioRecorder::wavData(int outputBits) const
{
    return wavFromCaptured(m_data, m_format, outputBits);
}

QByteArray AudioRecorder::pcm16Mono() const
{
    return pcm::mono16(m_data, m_format, 16000);
}

QVector<float> AudioRecorder::peaks(int count) const
{
    return pcm::peaks(pcm16Mono(), 1, count);
}
