// 音频设备枚举与按 id 查找（id 以十六进制字符串持久化）。
#pragma once

#include <QAudioDevice>
#include <QList>
#include <QString>

namespace audio {

QList<QAudioDevice> inputs();
QList<QAudioDevice> outputs();

QString deviceId(const QAudioDevice &device);
QString displayName(const QAudioDevice &device);

// id 为空时返回系统默认设备；找不到时回退到默认设备。
QAudioDevice findInput(const QString &id);
QAudioDevice findOutput(const QString &id);

} // namespace audio
