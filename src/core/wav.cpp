#include "core/wav.h"

#include <QtEndian>

namespace {

void appendU16(QByteArray &out, quint16 value)
{
    char buffer[2];
    qToLittleEndian(value, buffer);
    out.append(buffer, 2);
}

void appendU32(QByteArray &out, quint32 value)
{
    char buffer[4];
    qToLittleEndian(value, buffer);
    out.append(buffer, 4);
}

} // namespace

QByteArray wavFromPcm(const QByteArray &pcm, int sampleRate, int channels)
{
    const int bitsPerSample = 16;
    const int blockAlign = channels * bitsPerSample / 8;
    const quint32 dataSize = quint32(pcm.size());

    QByteArray out;
    out.reserve(44 + pcm.size());
    out.append("RIFF", 4);
    appendU32(out, 36 + dataSize);
    out.append("WAVE", 4);
    out.append("fmt ", 4);
    appendU32(out, 16);
    appendU16(out, 1); // PCM
    appendU16(out, quint16(channels));
    appendU32(out, quint32(sampleRate));
    appendU32(out, quint32(sampleRate * blockAlign));
    appendU16(out, quint16(blockAlign));
    appendU16(out, bitsPerSample);
    out.append("data", 4);
    appendU32(out, dataSize);
    out.append(pcm);
    return out;
}

QByteArray wavFromCaptured(const QByteArray &pcm, const QAudioFormat &format, int outputBits)
{
    const int channels = qMax(1, format.channelCount());
    const int sampleRate = qMax(1, format.sampleRate());
    if (outputBits != 16 && outputBits != 24 && outputBits != 32)
        outputBits = 16;

    const int inputBytes = format.bytesPerSample();
    const int frameBytes = inputBytes * channels;
    const bool input16 = format.sampleFormat() == QAudioFormat::Int16;
    const int frames = frameBytes > 0 ? pcm.size() / frameBytes : 0;
    const int outputBytes = outputBits / 8;
    const quint32 dataSize = quint32(frames * channels * outputBytes);

    QByteArray out;
    out.reserve(44 + int(dataSize));
    out.append("RIFF", 4);
    appendU32(out, 36 + dataSize);
    out.append("WAVE", 4);
    out.append("fmt ", 4);
    appendU32(out, 16);
    appendU16(out, 1); // PCM
    appendU16(out, quint16(channels));
    appendU32(out, quint32(sampleRate));
    const quint16 blockAlign = quint16(channels * outputBytes);
    appendU32(out, quint32(sampleRate * blockAlign));
    appendU16(out, blockAlign);
    appendU16(out, quint16(outputBits));
    out.append("data", 4);
    appendU32(out, dataSize);

    const uchar *base = reinterpret_cast<const uchar *>(pcm.constData());
    for (int frame = 0; frame < frames; ++frame) {
        const uchar *frameData = base + frame * frameBytes;
        for (int channel = 0; channel < channels; ++channel) {
            qint32 value = 0;
            if (input16) {
                value = qint32(qFromLittleEndian<qint16>(frameData + channel * inputBytes)) << 16;
            } else if (format.sampleFormat() == QAudioFormat::Int32) {
                value = qFromLittleEndian<qint32>(frameData + channel * inputBytes);
            }

            if (outputBits == 16) {
                const qint16 sample = qint16(value >> 16);
                char bytes[2];
                qToLittleEndian(sample, bytes);
                out.append(bytes, 2);
            } else if (outputBits == 24) {
                const quint32 packed = quint32(value) >> 8;
                char bytes[3] = {char(packed & 0xFF), char((packed >> 8) & 0xFF),
                                 char((packed >> 16) & 0xFF)};
                out.append(bytes, 3);
            } else {
                char bytes[4];
                qToLittleEndian(value, bytes);
                out.append(bytes, 4);
            }
        }
    }
    return out;
}
