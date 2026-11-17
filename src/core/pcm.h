// PCM 转换：任意 Qt 音频格式 → 单声道 float / 16-bit PCM，线性重采样。
#pragma once

#include <QAudioFormat>
#include <QByteArray>
#include <QVector>

namespace pcm {

QVector<float> toMonoFloat(const QByteArray &data, const QAudioFormat &format);
QVector<float> resample(const QVector<float> &samples, int sourceRate, int targetRate);
QByteArray toPcm16(const QVector<float> &samples);

// 转成目标采样率的单声道 16-bit PCM（小端）。
QByteArray mono16(const QByteArray &data, const QAudioFormat &format, int targetRate);

// 从单声道 16-bit PCM 生成 count 个 0..1 的峰值，用于卡片波形。
QVector<float> peaks(const QByteArray &pcm16, int channels, int count);

} // namespace pcm
