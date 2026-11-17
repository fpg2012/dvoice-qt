#pragma once

#include <QAudioFormat>
#include <QByteArray>

// 把 16 位小端 PCM 包成标准 WAV（单声道/多声道均可）。
QByteArray wavFromPcm(const QByteArray &pcm, int sampleRate, int channels = 1);

// 把录音采集到的 PCM（Int16 或 Int32）导出成 16/24/32 位 WAV。
QByteArray wavFromCaptured(const QByteArray &pcm, const QAudioFormat &format, int outputBits);
