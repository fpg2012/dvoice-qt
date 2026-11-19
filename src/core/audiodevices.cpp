#include "core/audiodevices.h"

#include <QMediaDevices>

namespace audio {

namespace {

constexpr int kRateCandidates[] = {48000, 44100, 32000, 24000, 16000, 96000, 22050, 8000, 11025, 88200};
constexpr int kChannelCandidates[] = {1, 2};

QAudioFormat::SampleFormat formatForBits(int bits)
{
    return bits <= 16 ? QAudioFormat::Int16 : QAudioFormat::Int32;
}

int bitsForFormat(const QAudioFormat &format, int requestedBits)
{
    switch (format.sampleFormat()) {
    case QAudioFormat::Int16:
        return 16;
    case QAudioFormat::Int32:
        return requestedBits == 24 ? 24 : 32;
    case QAudioFormat::Float:
        return 32;
    default:
        return 16;
    }
}

QAudioFormat makeFormat(int sampleRate, int channels, QAudioFormat::SampleFormat sampleFormat)
{
    QAudioFormat format;
    format.setSampleRate(sampleRate);
    format.setChannelCount(channels);
    format.setSampleFormat(sampleFormat);
    return format;
}

ResolvedFormat resolve(const QAudioDevice &device, int sampleRate, int channels, int bits)
{
    ResolvedFormat result;
    if (device.isNull() || sampleRate <= 0 || channels <= 0)
        return result;

    const QAudioFormat requested = makeFormat(sampleRate, channels, formatForBits(bits));
    if (device.isFormatSupported(requested)) {
        result.format = requested;
        result.sampleRate = sampleRate;
        result.channels = channels;
        result.bits = bits;
        return result;
    }

    QList<QAudioFormat> candidates;
    auto add = [&candidates](const QAudioFormat &format) {
        if (format.isValid() && !candidates.contains(format))
            candidates.append(format);
    };

    // 保持请求的采样率/声道，先退到位深最通用的 16-bit。
    add(requested);
    add(makeFormat(sampleRate, channels, QAudioFormat::Int16));
    // 设备自己推荐的格式通常最稳。
    add(device.preferredFormat());
    // 最后按候选网格兜底：采样率优先接近请求值，声道尽量少，位深从 16-bit 起。
    for (const int rate : kRateCandidates) {
        for (const int channelCount : kChannelCandidates) {
            add(makeFormat(rate, channelCount, QAudioFormat::Int16));
            add(makeFormat(rate, channelCount, QAudioFormat::Int32));
            add(makeFormat(rate, channelCount, QAudioFormat::Float));
        }
    }
    add(makeFormat(sampleRate, channels, QAudioFormat::Int32));
    add(makeFormat(sampleRate, channels, QAudioFormat::Float));

    for (const QAudioFormat &candidate : candidates) {
        if (!device.isFormatSupported(candidate))
            continue;
        result.format = candidate;
        result.sampleRate = candidate.sampleRate();
        result.channels = candidate.channelCount();
        result.bits = bitsForFormat(candidate, bits);
        result.adjusted = candidate != requested;
        return result;
    }
    return result;
}

} // namespace

QList<QAudioDevice> inputs()
{
    return QMediaDevices::audioInputs();
}

QList<QAudioDevice> outputs()
{
    return QMediaDevices::audioOutputs();
}

QString deviceId(const QAudioDevice &device)
{
    return device.isNull() ? QString() : QString::fromLatin1(device.id().toHex());
}

QString displayName(const QAudioDevice &device)
{
    return device.isNull() ? QStringLiteral("系统默认") : device.description();
}

QAudioDevice findInput(const QString &id)
{
    if (!id.isEmpty()) {
        for (const QAudioDevice &device : inputs()) {
            if (deviceId(device) == id)
                return device;
        }
    }
    return QMediaDevices::defaultAudioInput();
}

QAudioDevice findOutput(const QString &id)
{
    if (!id.isEmpty()) {
        for (const QAudioDevice &device : outputs()) {
            if (deviceId(device) == id)
                return device;
        }
    }
    return QMediaDevices::defaultAudioOutput();
}

ResolvedFormat resolveInputFormat(const QAudioDevice &device, int sampleRate, int channels,
                                  int bits)
{
    return resolve(device, sampleRate, channels, bits);
}

ResolvedFormat resolveOutputFormat(const QAudioDevice &device, int sampleRate, int channels,
                                   int bits)
{
    return resolve(device, sampleRate, channels, bits);
}

QString formatText(int sampleRate, int channels, int bits)
{
    return QStringLiteral("%1 Hz / %2 声道 / %3 位").arg(sampleRate).arg(channels).arg(bits);
}

} // namespace audio
