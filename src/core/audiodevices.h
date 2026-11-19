// 音频设备枚举、按 id 查找（id 以十六进制字符串持久化），以及录音/放音格式协商。
#pragma once

#include <QAudioDevice>
#include <QAudioFormat>
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

// 设备实际可用的格式。Windows（WASAPI）只接受设备原生格式，而 Qt 往往只报告
// 48 kHz + Float32；Linux 的 PulseAudio 则会自动重采样。故这里不假设任何格式，
// 而是按「请求值 → 设备 preferredFormat → 候选网格」逐步探测。
struct ResolvedFormat {
    QAudioFormat format;   // 可直接交给 QAudioSource / QAudioSink；无效表示无可用格式
    int sampleRate = 0;
    int channels = 0;
    int bits = 0;          // 16 / 24 / 32，与 format 的采样格式对应
    bool adjusted = false; // true 表示与请求值不同
};

ResolvedFormat resolveInputFormat(const QAudioDevice &device, int sampleRate, int channels,
                                  int bits);
ResolvedFormat resolveOutputFormat(const QAudioDevice &device, int sampleRate, int channels,
                                   int bits);

// 「48000 Hz / 1 声道 / 32 位」。
QString formatText(int sampleRate, int channels, int bits);

} // namespace audio
