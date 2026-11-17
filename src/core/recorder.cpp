#include "core/recorder.h"

#include <QAudioSource>
#include <QIODevice>
#include <QTimer>
#include <QtEndian>

#include <cmath>

namespace {
constexpr int kFrameSamples = 320; // 20 ms @ 16 kHz
constexpr int kFrameBytes = kFrameSamples * 2;
constexpr int kPrerollFrames = 15; // 300 ms
constexpr int kFrameMs = 20;
} // namespace

Recorder::Recorder(QObject *parent)
    : QObject(parent)
{
}

Recorder::~Recorder()
{
    abort();
}

QAudioFormat Recorder::format()
{
    QAudioFormat format;
    format.setSampleRate(16000);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);
    return format;
}

void Recorder::resetState()
{
    m_pending.clear();
    m_segment.clear();
    m_preroll.clear();
    m_prerollMs.clear();
    m_totalSamples = 0;
    m_elapsedMs = 0;
    m_speechStartMs = 0;
    m_silenceFrames = 0;
    m_voiceFrames = 0;
    m_noise = 0.0015;
    m_threshold = 0.006;
    m_speaking = false;
}

bool Recorder::start(const QAudioDevice &device, const VadConfig &config, QString *error)
{
    stop();
    resetState();
    m_config = config;
    m_device = device;

    if (device.isNull()) {
        if (error)
            *error = QStringLiteral("没有可用的输入设备");
        return false;
    }
    if (!device.isFormatSupported(format())) {
        if (error) {
            *error = QStringLiteral("「%1」不支持 16 kHz 单声道录音")
                         .arg(device.description());
        }
        return false;
    }

    m_source = new QAudioSource(device, format(), this);
    m_source->setBufferSize(format().bytesForDuration(100000)); // 100 ms
    m_stopping = false;
    m_io = m_source->start();
    if (!m_io || m_source->error() != QAudio::NoError) {
        if (error)
            *error = QStringLiteral("无法打开麦克风：%1").arg(device.description());
        m_source->deleteLater();
        m_source = nullptr;
        m_io = nullptr;
        return false;
    }

    connect(m_io, &QIODevice::readyRead, this, &Recorder::handleReadyRead);
    connect(m_source, &QAudioSource::stateChanged, this, [this](QAudio::State state) {
        if (state == QAudio::StoppedState && m_source && !m_stopping
            && m_source->error() != QAudio::NoError) {
            emit errorOccurred(QStringLiteral("录音设备异常：%1").arg(m_device.description()));
        }
    });
    return true;
}

void Recorder::stop()
{
    if (!m_source)
        return;
    if (m_io)
        m_pending.append(m_io->readAll());
    if (m_speaking)
        finishSegment(false);

    m_stopping = true;
    m_source->stop();
    m_source->disconnect(this);
    m_source->deleteLater();
    m_source = nullptr;
    m_io = nullptr;
    resetState();
}

void Recorder::abort()
{
    const bool wasSpeaking = m_speaking;
    if (!m_source)
        return;
    m_stopping = true;
    m_source->stop();
    m_source->disconnect(this);
    m_source->deleteLater();
    m_source = nullptr;
    m_io = nullptr;
    resetState();
    if (wasSpeaking)
        emit speechChanged(false);
}

void Recorder::beginTestFeed(const VadConfig &config)
{
    abort();
    resetState();
    m_config = config;
}

void Recorder::feedTestPcm(const QByteArray &pcm)
{
    m_pending.append(pcm);
    while (m_pending.size() >= kFrameBytes) {
        processFrame(m_pending.constData());
        m_pending.remove(0, kFrameBytes);
    }
}

void Recorder::endTestFeed()
{
    if (m_speaking)
        finishSegment(false);
    resetState();
}

void Recorder::handleReadyRead()
{
    if (!m_io)
        return;
    m_pending.append(m_io->readAll());
    while (m_pending.size() >= kFrameBytes) {
        processFrame(m_pending.constData());
        m_pending.remove(0, kFrameBytes);
    }
}

void Recorder::processFrame(const char *data)
{
    qint64 sum = 0;
    int peak = 0;
    for (int i = 0; i < kFrameSamples; ++i) {
        const qint16 sample = qFromLittleEndian<qint16>(data + i * 2);
        sum += qint64(sample) * sample;
        peak = qMax(peak, qAbs(int(sample)));
    }
    const qreal rms = std::sqrt(qreal(sum) / kFrameSamples) / 32768.0;
    emit level(rms, qreal(peak) / 32768.0);

    const QByteArray frame(data, kFrameBytes);
    emit pcmChunk(frame);

    if (!m_speaking) {
        m_preroll.push_back(frame);
        m_prerollMs.push_back(m_elapsedMs);
        while (int(m_preroll.size()) > kPrerollFrames) {
            m_preroll.pop_front();
            m_prerollMs.pop_front();
        }

        m_noise = m_noise * 0.995 + rms * 0.005;
        m_threshold = qBound(0.004, m_noise * 3.0, 0.05);

        if (rms > m_threshold) {
            if (++m_voiceFrames >= 2)
                beginSpeech();
        } else {
            m_voiceFrames = 0;
        }
    } else {
        m_segment.append(frame);
        if (rms < m_threshold * 0.7)
            ++m_silenceFrames;
        else
            m_silenceFrames = 0;

        if (m_silenceFrames * kFrameMs >= m_config.silenceMs) {
            finishSegment(false);
        } else if (m_elapsedMs - m_speechStartMs >= m_config.maxSegmentMs) {
            // 到达单段上限：先交出去，继续听下一段。
            const int endMs = m_elapsedMs;
            if (endMs - m_speechStartMs >= m_config.minSpeechMs && !m_segment.isEmpty())
                emit segment(m_segment, m_speechStartMs, endMs);
            m_segment.clear();
            m_speechStartMs = m_elapsedMs;
            m_silenceFrames = 0;
        }
    }

    m_totalSamples += kFrameSamples;
    m_elapsedMs += kFrameMs;
}

void Recorder::beginSpeech()
{
    if (m_prerollMs.empty())
        return;
    m_speechStartMs = m_prerollMs.front();
    m_segment.clear();
    for (const QByteArray &frame : m_preroll)
        m_segment.append(frame);
    m_preroll.clear();
    m_prerollMs.clear();
    m_speaking = true;
    m_silenceFrames = 0;
    m_voiceFrames = 0;
    emit speechChanged(true);
}

void Recorder::finishSegment(bool force)
{
    const int endMs = m_elapsedMs;
    const int duration = endMs - m_speechStartMs;
    if (!m_segment.isEmpty() && (duration >= m_config.minSpeechMs || force))
        emit segment(m_segment, m_speechStartMs, endMs);

    m_segment.clear();
    m_speaking = false;
    m_silenceFrames = 0;
    m_voiceFrames = 0;
    emit speechChanged(false);
}
