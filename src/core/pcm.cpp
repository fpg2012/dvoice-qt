#include "core/pcm.h"

#include <QtEndian>

#include <cmath>
#include <cstring>

namespace {

float readSample(const uchar *data, QAudioFormat::SampleFormat format)
{
    switch (format) {
    case QAudioFormat::UInt8:
        return (int(data[0]) - 128) / 128.0f;
    case QAudioFormat::Int16:
        return qFromLittleEndian<qint16>(data) / 32768.0f;
    case QAudioFormat::Int32:
        return qFromLittleEndian<qint32>(data) / 2147483648.0f;
    case QAudioFormat::Float: {
        float value = 0.0f;
        std::memcpy(&value, data, sizeof(float));
        return value;
    }
    default:
        return 0.0f;
    }
}

} // namespace

namespace pcm {

QVector<float> toMonoFloat(const QByteArray &data, const QAudioFormat &format)
{
    QVector<float> result;
    if (!format.isValid() || data.isEmpty())
        return result;

    const int channels = format.channelCount();
    const int bytesPerSample = format.bytesPerSample();
    const int frameBytes = bytesPerSample * channels;
    if (frameBytes <= 0)
        return result;

    const int frames = data.size() / frameBytes;
    result.reserve(frames);
    const uchar *base = reinterpret_cast<const uchar *>(data.constData());
    for (int frame = 0; frame < frames; ++frame) {
        const uchar *frameData = base + frame * frameBytes;
        float sum = 0.0f;
        for (int channel = 0; channel < channels; ++channel) {
            sum += readSample(frameData + channel * bytesPerSample,
                              format.sampleFormat());
        }
        result.append(sum / channels);
    }
    return result;
}

QVector<float> resample(const QVector<float> &samples, int sourceRate, int targetRate)
{
    if (samples.isEmpty() || sourceRate <= 0 || targetRate <= 0
        || sourceRate == targetRate) {
        return samples;
    }

    const qint64 outputFrames = qint64(samples.size()) * targetRate / sourceRate;
    QVector<float> result;
    result.reserve(int(qMax<qint64>(0, outputFrames)));
    for (qint64 i = 0; i < outputFrames; ++i) {
        const double position = double(i) * sourceRate / targetRate;
        const qint64 index = qint64(position);
        const double fraction = position - index;
        const float a = samples[int(qBound<qint64>(0, index, samples.size() - 1))];
        const float b = samples[int(qBound<qint64>(0, index + 1, samples.size() - 1))];
        result.append(float(a + (b - a) * fraction));
    }
    return result;
}

QByteArray toPcm16(const QVector<float> &samples)
{
    QByteArray result;
    result.reserve(samples.size() * 2);
    for (const float value : samples) {
        const float clamped = qBound(-1.0f, value, 1.0f);
        const qint16 sample = qint16(std::lround(clamped * 32767.0f));
        char bytes[2];
        qToLittleEndian(sample, bytes);
        result.append(bytes, 2);
    }
    return result;
}

QByteArray mono16(const QByteArray &data, const QAudioFormat &format, int targetRate)
{
    const QVector<float> mono = toMonoFloat(data, format);
    return toPcm16(resample(mono, format.sampleRate(), targetRate));
}

QVector<float> peaks(const QByteArray &pcm16, int channels, int count)
{
    QVector<float> result(qMax(1, count), 0.0f);
    if (pcm16.size() < 2 || channels <= 0)
        return result;

    const int frames = pcm16.size() / (2 * channels);
    if (frames <= 0)
        return result;

    const auto *samples = reinterpret_cast<const qint16 *>(pcm16.constData());
    for (int frame = 0; frame < frames; ++frame) {
        float sum = 0.0f;
        for (int channel = 0; channel < channels; ++channel)
            sum += qAbs(int(samples[frame * channels + channel])) / 32768.0f;
        const float value = sum / channels;
        const int bucket = qBound(0, frame * result.size() / frames, result.size() - 1);
        result[bucket] = qMax(result[bucket], value);
    }
    return result;
}

} // namespace pcm
