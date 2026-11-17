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
