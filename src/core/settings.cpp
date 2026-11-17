#include "core/settings.h"

#include <QDir>
#include <QFile>
#include <QSettings>
#include <QStringList>

namespace {

QString trimmedBase(const QString &value)
{
    QString url = value.trimmed();
    while (url.endsWith(QLatin1Char('/')))
        url.chop(1);
    return url;
}

QString readDshKey()
{
    QFile file(QDir::homePath() + QStringLiteral("/.dsh/.credentials.yaml"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (!trimmed.startsWith(QLatin1String("DEEPSEEK_API_KEY:")))
            continue;

        QString value = trimmed.mid(int(qstrlen("DEEPSEEK_API_KEY:"))).trimmed();
        if (value.size() >= 2
            && ((value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))
                || (value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\''))))) {
            value = value.mid(1, value.size() - 2);
        }
        if (value.startsWith(QLatin1String("${")))
            return {};
        if (!value.isEmpty())
            return value;
    }
    return {};
}

} // namespace

Settings Settings::defaults()
{
    return Settings{};
}

Settings Settings::load()
{
    QSettings store;
    Settings s;
    s.apiKey = store.value(QStringLiteral("api/key")).toString();
    s.rememberApiKey = store.value(QStringLiteral("api/remember"), true).toBool();
    s.baseUrl = trimmedBase(store.value(QStringLiteral("api/baseUrl"), s.baseUrl).toString());
    s.inputDevice = store.value(QStringLiteral("audio/input")).toString();
    s.outputDevice = store.value(QStringLiteral("audio/output")).toString();
    s.voiceId = store.value(QStringLiteral("tts/voice"), s.voiceId).toString();
    s.filterMarkdown = store.value(QStringLiteral("tts/filterMarkdown"), true).toBool();
    s.silenceMs = store.value(QStringLiteral("vad/silenceMs"), s.silenceMs).toInt();
    s.minSpeechMs = store.value(QStringLiteral("vad/minSpeechMs"), s.minSpeechMs).toInt();
    s.maxSegmentMs = store.value(QStringLiteral("vad/maxSegmentMs"), s.maxSegmentMs).toInt();
    s.volume = store.value(QStringLiteral("audio/volume"), s.volume).toInt();
    s.recordSampleRate = store.value(QStringLiteral("record/sampleRate"), s.recordSampleRate).toInt();
    s.recordChannels = store.value(QStringLiteral("record/channels"), s.recordChannels).toInt();
    s.recordBits = store.value(QStringLiteral("record/bits"), s.recordBits).toInt();
    if (s.baseUrl.isEmpty())
        s.baseUrl = defaults().baseUrl;
    if (s.voiceId != QLatin1String("stella") && s.voiceId != QLatin1String("tide"))
        s.voiceId = QStringLiteral("stella");
    if (s.recordSampleRate <= 0)
        s.recordSampleRate = 48000;
    s.recordChannels = s.recordChannels == 2 ? 2 : 1;
    if (s.recordBits != 16 && s.recordBits != 24 && s.recordBits != 32)
        s.recordBits = 24;
    return s;
}

void Settings::save() const
{
    QSettings store;
    store.setValue(QStringLiteral("api/remember"), rememberApiKey);
    if (rememberApiKey)
        store.setValue(QStringLiteral("api/key"), apiKey);
    else
        store.remove(QStringLiteral("api/key"));
    store.setValue(QStringLiteral("api/baseUrl"), trimmedBase(baseUrl));
    store.setValue(QStringLiteral("audio/input"), inputDevice);
    store.setValue(QStringLiteral("audio/output"), outputDevice);
    store.setValue(QStringLiteral("tts/voice"), voiceId);
    store.setValue(QStringLiteral("tts/filterMarkdown"), filterMarkdown);
    store.setValue(QStringLiteral("vad/silenceMs"), silenceMs);
    store.setValue(QStringLiteral("vad/minSpeechMs"), minSpeechMs);
    store.setValue(QStringLiteral("vad/maxSegmentMs"), maxSegmentMs);
    store.setValue(QStringLiteral("audio/volume"), volume);
    store.setValue(QStringLiteral("record/sampleRate"), recordSampleRate);
    store.setValue(QStringLiteral("record/channels"), recordChannels);
    store.setValue(QStringLiteral("record/bits"), recordBits);
}

QString Settings::discoverApiKey()
{
    const QByteArray env = qgetenv("DEEPSEEK_API_KEY");
    if (!env.isEmpty())
        return QString::fromUtf8(env).trimmed();
    return readDshKey();
}

QString Settings::effectiveApiKey() const
{
    if (!apiKey.trimmed().isEmpty())
        return apiKey.trimmed();
    return discoverApiKey();
}

QString Settings::endpoint() const
{
    return trimmedBase(baseUrl.isEmpty() ? defaults().baseUrl : baseUrl);
}
