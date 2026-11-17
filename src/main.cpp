#include "core/asr.h"
#include "core/audiodevices.h"
#include "core/player.h"
#include "core/recorder.h"
#include "core/settings.h"
#include "core/tts.h"
#include "core/wav.h"
#include "mainwindow.h"
#include "preferences.h"
#include "theme.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDialog>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QTimer>

#include <cmath>

namespace {

int reportError(const QString &message)
{
    QTextStream(stderr) << message << '\n';
    return 1;
}

int probeTts(const QString &text, const QString &outPath, const QString &voice)
{
    Settings settings = Settings::load();
    const QString key = settings.effectiveApiKey();
    if (key.isEmpty())
        return reportError(QStringLiteral("没有可用的 API Key"));
    if (text.trimmed().isEmpty())
        return reportError(QStringLiteral("文本为空"));

    tts::Request request;
    request.text = text;
    request.baseUrl = settings.endpoint();
    request.apiKey = key;
    request.voiceId = voice.isEmpty() ? settings.voiceId : voice;
    request.filterMarkdown = settings.filterMarkdown;

    tts::Client client;
    QByteArray pcm;
    QElapsedTimer timer;
    qint64 firstChunkMs = -1;
    QString failure;
    bool done = false;

    QEventLoop loop;
    QObject::connect(&client, &tts::Client::audioChunk, &loop, [&](const QByteArray &data) {
        if (firstChunkMs < 0)
            firstChunkMs = timer.elapsed();
        pcm.append(data);
    });
    QObject::connect(&client, &tts::Client::finished, &loop, [&] {
        done = true;
        loop.quit();
    });
    QObject::connect(&client, &tts::Client::failed, &loop, [&](const QString &message) {
        failure = message;
        done = true;
        loop.quit();
    });

    timer.start();
    client.start(request);
    if (!done) {
        QTimer::singleShot(180000, &loop, [&] {
            failure = QStringLiteral("等待合成结果超时");
            loop.quit();
        });
        loop.exec();
    }

    if (!failure.isEmpty())
        return reportError(QStringLiteral("合成失败：%1").arg(failure));
    if (pcm.isEmpty())
        return reportError(QStringLiteral("服务未返回音频"));

    const QString path = outPath.isEmpty() ? QStringLiteral("dvoice-tts.wav") : outPath;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return reportError(QStringLiteral("无法写入 %1：%2").arg(path, file.errorString()));
    file.write(wavFromPcm(pcm, 24000));
    file.close();

    const double seconds = pcm.size() / 2.0 / 24000.0;
    QTextStream(stdout) << QStringLiteral("合成 %1 字符（计费 %2）· %3 秒音频 · 首包 %4 ms\n")
                               .arg(text.size())
                               .arg(tts::billedChars(text))
                               .arg(seconds, 0, 'f', 2)
                               .arg(firstChunkMs)
                        << QStringLiteral("已写入 %1（%2 字节）\n").arg(path).arg(pcm.size());
    return 0;
}

QString playerStateName(Player::State state)
{
    switch (state) {
    case Player::State::Idle:
        return QStringLiteral("Idle");
    case Player::State::Buffering:
        return QStringLiteral("Buffering");
    case Player::State::Playing:
        return QStringLiteral("Playing");
    case Player::State::Paused:
        return QStringLiteral("Paused");
    case Player::State::Finished:
        return QStringLiteral("Finished");
    case Player::State::Failed:
        return QStringLiteral("Failed");
    }
    return QStringLiteral("?");
}

int probePlayback(const QString &text, bool seekTest)
{
    Settings settings = Settings::load();
    const QString key = settings.effectiveApiKey();
    if (key.isEmpty())
        return reportError(QStringLiteral("没有可用的 API Key"));

    tts::Request request;
    request.baseUrl = settings.endpoint();
    request.apiKey = key;
    request.voiceId = settings.voiceId;
    request.filterMarkdown = false;

    Player player;
    player.setOutputDevice(audio::findOutput(settings.outputDevice));
    player.setVolume(0.6);

    QEventLoop loop;
    bool done = false;
    bool seeked = false;
    QString failure;
    qint64 firstAudioMs = -1;
    qint64 bytes = 0;
    QElapsedTimer timer;
    timer.start();

    QObject::connect(&player, &Player::stateChanged, &loop, [&](Player::State state) {
        QTextStream(stdout) << QStringLiteral("状态 %1\n").arg(playerStateName(state));
        if (state == Player::State::Failed) {
            done = true;
            loop.quit();
        } else if (state == Player::State::Finished) {
            if (seekTest && !seeked) {
                seeked = true;
                QTextStream(stdout) << QStringLiteral("首次播放完成，定位到 800 ms\n");
                player.seek(800);
                return;
            }
            done = true;
            loop.quit();
        }
    });
    QObject::connect(&player, &Player::audioChunk, &loop, [&](const QByteArray &pcm) {
        if (firstAudioMs < 0)
            firstAudioMs = timer.elapsed();
        bytes += pcm.size();
    });
    QObject::connect(&player, &Player::errorOccurred, &loop, [&](const QString &message) {
        failure = message;
        done = true;
        loop.quit();
    });

    player.play(text, request);
    if (!done) {
        QTimer::singleShot(120000, &loop, [&] {
            failure = QStringLiteral("等待播放完成超时");
            loop.quit();
        });
        loop.exec();
    }

    if (!failure.isEmpty())
        return reportError(QStringLiteral("播放失败：%1").arg(failure));
    if (!done)
        return reportError(QStringLiteral("等待播放完成超时"));

    QTextStream(stdout) << QStringLiteral("播放完成 · %1 秒音频 · 首包 %2 ms · %3 字节\n")
                               .arg(player.durationMs() / 1000.0, 0, 'f', 2)
                               .arg(firstAudioMs)
                               .arg(bytes);
    return 0;
}

int probeVad()
{
    Recorder recorder;
    QVector<QPair<int, int>> segments;
    QObject::connect(&recorder, &Recorder::segment, &recorder,
                     [&](const QByteArray &pcm, int start, int end) {
                         segments.append({start, end});
                         QTextStream(stdout)
                             << QStringLiteral("段 %1–%2 ms · %3 ms 音频\n")
                                    .arg(start).arg(end).arg(pcm.size() / 32);
                     });

    VadConfig config;
    config.silenceMs = 650;
    config.minSpeechMs = 300;
    config.maxSegmentMs = 15000;
    recorder.beginTestFeed(config);

    const auto silence = [&](int ms) { recorder.feedTestPcm(QByteArray(ms * 32, 0)); };
    const auto tone = [&](int ms) {
        QByteArray pcm;
        pcm.reserve(ms * 32);
        for (int i = 0; i < ms * 16; ++i) {
            const double t = double(i) / 16000.0;
            const double value = 0.15 * std::sin(2.0 * M_PI * 220.0 * t)
                + 0.05 * std::sin(2.0 * M_PI * 660.0 * t);
            const qint16 sample = qint16(value * 32767.0);
            char bytes[2];
            qToLittleEndian(sample, bytes);
            pcm.append(bytes, 2);
        }
        recorder.feedTestPcm(pcm);
    };

    silence(500);
    tone(1200);
    silence(900);
    tone(1200);
    silence(900);
    recorder.endTestFeed();

    QTextStream(stdout) << QStringLiteral("VAD 共切出 %1 段\n").arg(segments.size());
    return segments.size() == 2 ? 0 : 1;
}

int probeAsr(const QString &path)
{    Settings settings = Settings::load();
    const QString key = settings.effectiveApiKey();
    if (key.isEmpty())
        return reportError(QStringLiteral("没有可用的 API Key"));

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return reportError(QStringLiteral("无法读取 %1").arg(path));
    const QByteArray wav = file.readAll();

    asr::Client client;
    client.setEndpoint(settings.endpoint(), key);
    asr::Result result;
    bool done = false;
    QEventLoop loop;
    QObject::connect(&client, &asr::Client::finished, &loop, [&](const asr::Result &value) {
        result = value;
        done = true;
        loop.quit();
    });

    QElapsedTimer timer;
    timer.start();
    client.transcribe(wav);
    if (!done) {
        QTimer::singleShot(180000, &loop, [&] { loop.quit(); });
        loop.exec();
    }
    if (!done)
        return reportError(QStringLiteral("等待识别结果超时"));
    if (!result.ok())
        return reportError(QStringLiteral("识别失败：%1").arg(result.error));

    QTextStream(stdout) << QStringLiteral("识别完成 · %1 ms 音频 · %2 ms 耗时\n")
                               .arg(result.durationMs)
                               .arg(timer.elapsed())
                        << result.text << '\n';
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("dvoice-qt"));
    QApplication::setApplicationDisplayName(QStringLiteral("dvoice"));
    QApplication::setOrganizationName(QStringLiteral("dvoice-qt"));
    QApplication::setDesktopFileName(QStringLiteral("dvoice-qt"));
    QApplication::setWindowIcon(theme::appIcon());
    QApplication::setApplicationVersion(QStringLiteral(DV_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("DeepSeek 语音识别与语音合成桌面客户端"));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption darkOption(QStringLiteral("dark"), QStringLiteral("强制深色方案（调试用）"));
    QCommandLineOption lightOption(QStringLiteral("light"),
                                   QStringLiteral("强制浅色方案（调试用）"));
    QCommandLineOption screenshotOption(QStringLiteral("screenshot"),
                                        QStringLiteral("保存窗口截图并退出（调试用）"),
                                        QStringLiteral("png"));
    QCommandLineOption demoOption(QStringLiteral("demo"),
                                  QStringLiteral("填入示例文本与波形（离线检查界面）"));
    QCommandLineOption prefsOption(QStringLiteral("prefs"), QStringLiteral("启动后打开首选项"));
    QCommandLineOption prefsTabOption(QStringLiteral("prefs-tab"),
                                      QStringLiteral("调试：打开首选项并切到指定标签页（0 起）"),
                                      QStringLiteral("index"));
    QCommandLineOption aboutOption(QStringLiteral("about"), QStringLiteral("启动后打开关于"));
    QCommandLineOption micTestOption(QStringLiteral("mic-test"),
                                     QStringLiteral("启动后打开麦克风测试"));
    QCommandLineOption speakerTestOption(QStringLiteral("speaker-test"),
                                         QStringLiteral("启动后打开扬声器测试"));
    QCommandLineOption probeTtsOption(QStringLiteral("probe-tts"),
                                      QStringLiteral("调试：合成一段文本并写出 WAV"),
                                      QStringLiteral("text"));
    QCommandLineOption probeAsrOption(QStringLiteral("probe-asr"),
                                      QStringLiteral("调试：识别一个 WAV 文件并打印文本"),
                                      QStringLiteral("wav"));
    QCommandLineOption probePlayOption(QStringLiteral("probe-play"),
                                       QStringLiteral("调试：合成并实际播放一段文本"),
                                       QStringLiteral("text"));
    QCommandLineOption probeSeekOption(QStringLiteral("probe-seek"),
                                       QStringLiteral("调试：播放完成后拖动定位并继续播放"),
                                       QStringLiteral("text"));
    QCommandLineOption probeVadOption(QStringLiteral("probe-vad"),
                                      QStringLiteral("调试：用合成音频验证 VAD 分段"));
    QCommandLineOption outOption(QStringLiteral("out"),
                                 QStringLiteral("调试：--probe-tts 的输出路径"),
                                 QStringLiteral("path"));
    QCommandLineOption voiceOption(QStringLiteral("voice"),
                                   QStringLiteral("调试：--probe-tts 使用的音色"),
                                   QStringLiteral("stella|tide"));

    parser.addOptions({darkOption, lightOption, screenshotOption, demoOption, prefsOption, prefsTabOption,
                       aboutOption, micTestOption, speakerTestOption, probeTtsOption,
                       probeAsrOption, probePlayOption, probeSeekOption, probeVadOption, outOption, voiceOption});
    parser.process(app);

    if (parser.isSet(probeTtsOption))
        return probeTts(parser.value(probeTtsOption), parser.value(outOption),
                        parser.value(voiceOption));
    if (parser.isSet(probeAsrOption))
        return probeAsr(parser.value(probeAsrOption));
    if (parser.isSet(probePlayOption))
        return probePlayback(parser.value(probePlayOption), false);
    if (parser.isSet(probeSeekOption))
        return probePlayback(parser.value(probeSeekOption), true);
    if (parser.isSet(probeVadOption))
        return probeVad();

    if (parser.isSet(darkOption))
        theme::applyScheme(true);
    else if (parser.isSet(lightOption))
        theme::applyScheme(false);

    app.setStyleSheet(theme::styleSheet(app.palette()));

    MainWindow window;
    window.show();

    if (parser.isSet(demoOption))
        window.showDemo();
    if (parser.isSet(prefsOption) || parser.isSet(prefsTabOption)) {
        const int tab = parser.isSet(prefsTabOption) ? parser.value(prefsTabOption).toInt() : 0;
        QTimer::singleShot(200, &window, [&window, tab] {
            if (PreferencesDialog *dialog = window.openPreferences())
                dialog->selectTab(tab);
        });
    }
    if (parser.isSet(micTestOption))
        QTimer::singleShot(200, &window, [&window] { window.openMicTest(); });
    if (parser.isSet(speakerTestOption))
        QTimer::singleShot(200, &window, [&window] { window.openSpeakerTest(); });
    if (parser.isSet(aboutOption))
        QTimer::singleShot(200, &window, [&window] { window.openAbout(); });

    const QString screenshot = parser.value(screenshotOption);
    if (!screenshot.isEmpty()) {
        QTimer::singleShot(1100, &app, [&window, screenshot] {
            QWidget *target = QApplication::activeModalWidget();
            if (!target) {
                const QWidgetList widgets = QApplication::topLevelWidgets();
                for (QWidget *widget : widgets) {
                    if (widget != &window && widget->isVisible()
                        && qobject_cast<QDialog *>(widget)) {
                        target = widget;
                        break;
                    }
                }
            }
            if (!target)
                target = QApplication::activeWindow();
            if (!target)
                target = &window;

            if (!target->grab().save(screenshot))
                QTextStream(stderr) << QStringLiteral("无法保存截图到 %1\n").arg(screenshot);

            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
                dialog->close();
            QApplication::quit();
        });
    }

    return app.exec();
}
