#pragma once

#include <QByteArray>

// 把 16 位小端 PCM 包成标准 WAV（单声道/多声道均可）。
QByteArray wavFromPcm(const QByteArray &pcm, int sampleRate, int channels = 1);
