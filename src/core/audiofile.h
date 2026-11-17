// 音频文件解码：统一转成 16 kHz / 单声道 / 16-bit PCM，供识别与波形使用。
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace audiofile {

struct DecodedAudio {
    QByteArray pcm16;
    qint64 durationMs = 0;
    QString error;

    bool ok() const { return error.isEmpty() && !pcm16.isEmpty(); }
};

DecodedAudio decodeToMono16(const QString &path);

QStringList nameFilters();
bool isAudioFile(const QString &path);

} // namespace audiofile
