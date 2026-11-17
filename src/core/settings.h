// 应用设置：全部持久化在 QSettings；接口密钥可来自设置、环境变量或 DSH 凭据。
#pragma once

#include <QString>

struct Settings {
    QString apiKey;
    bool rememberApiKey = true;
    QString baseUrl = QStringLiteral("https://api.deepseek.com");

    // QAudioDevice::id() 的十六进制形式；空表示跟随系统默认设备。
    QString inputDevice;
    QString outputDevice;

    QString voiceId = QStringLiteral("stella");
    bool filterMarkdown = true;

    int silenceMs = 650;
    int minSpeechMs = 300;
    int maxSegmentMs = 15000;

    int volume = 80;

    // 录音（回放库）格式设置。位深为 16/24/32 位 PCM。
    int recordSampleRate = 48000;
    int recordChannels = 1;
    int recordBits = 24;
    double recordGainDb = 0.0; // dB，0 = 原始电平

    static Settings defaults();
    static Settings load();
    void save() const;

    // 设置里保存的密钥；如果为空，再尝试环境变量和 ~/.dsh/.credentials.yaml。
    QString effectiveApiKey() const;
    static QString discoverApiKey();

    QString endpoint() const;
};
