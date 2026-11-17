#include "core/audiofile.h"

#include "core/pcm.h"

#include <QAudioBuffer>
#include <QAudioDecoder>
#include <QAudioFormat>
#include <QEventLoop>
#include <QFileInfo>
#include <QTimer>
#include <QUrl>

namespace audiofile {

namespace {

QAudioFormat desiredFormat()
{
    QAudioFormat format;
    format.setSampleRate(16000);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);
    return format;
}

} // namespace

QStringList nameFilters()
{
    return {QStringLiteral("音频文件 (*.wav *.mp3 *.flac *.ogg *.opus *.m4a *.aac *.wma)"),
            QStringLiteral("所有文件 (*)")};
}

bool isAudioFile(const QString &path)
{
    static const QStringList suffixes = {
        QStringLiteral("wav"),  QStringLiteral("mp3"),  QStringLiteral("flac"),
        QStringLiteral("ogg"),  QStringLiteral("opus"), QStringLiteral("m4a"),
        QStringLiteral("aac"),  QStringLiteral("wma"),
    };
    return suffixes.contains(QFileInfo(path).suffix().toLower());
}

DecodedAudio decodeToMono16(const QString &path)
{
    DecodedAudio result;
    if (!QFileInfo::exists(path)) {
        result.error = QStringLiteral("文件不存在：%1").arg(path);
        return result;
    }

    QAudioDecoder decoder;
    const QAudioFormat desired = desiredFormat();
    decoder.setSource(QUrl::fromLocalFile(path));

    QEventLoop loop;
    bool done = false;
    bool failed = false;

    QObject::connect(&decoder, &QAudioDecoder::bufferReady, &loop, [&] {
        const QAudioBuffer buffer = decoder.read();
        if (!buffer.isValid())
            return;
        const QAudioFormat format = buffer.format();
        const QByteArray data(reinterpret_cast<const char *>(buffer.constData<uchar>()),
                              int(buffer.byteCount()));
        if (data.isEmpty())
            return;
        if (format.sampleRate() == desired.sampleRate()
            && format.channelCount() == 1
            && format.sampleFormat() == QAudioFormat::Int16) {
            result.pcm16.append(data);
        } else {
            result.pcm16.append(pcm::mono16(data, format, 16000));
        }
    });

    QObject::connect(&decoder, &QAudioDecoder::finished, &loop, [&] {
        done = true;
        loop.quit();
    });
    QObject::connect(&decoder, QOverload<QAudioDecoder::Error>::of(&QAudioDecoder::error),
                     &loop, [&](QAudioDecoder::Error) {
                         failed = true;
                         result.error = decoder.errorString();
                         loop.quit();
                     });

    decoder.start();
    if (!done && !failed) {
        QTimer::singleShot(600000, &loop, [&] {
            result.error = QStringLiteral("解码超时");
            loop.quit();
        });
        loop.exec();
    }
    decoder.stop();

    if (result.error.isEmpty() && result.pcm16.isEmpty())
        result.error = QStringLiteral("没有解码出音频数据");
    result.durationMs = result.pcm16.size() / 32;
    return result;
}

} // namespace audiofile
