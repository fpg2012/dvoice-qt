#include "core/recordingstore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace {

struct WavInfo {
    qint64 durationMs = 0;
    int sampleRate = 0;
    int channels = 1;
    int bits = 16;
};

WavInfo parseWavInfo(const QString &path)
{
    WavInfo info;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return info;
    const QByteArray header = file.read(4096);
    if (header.size() < 44 || !header.startsWith("RIFF") || header.mid(8, 4) != "WAVE")
        return info;

    const auto read16 = [&header](int pos) {
        return quint16(quint8(header[pos]) | (quint16(quint8(header[pos + 1])) << 8));
    };
    const auto read32 = [&header](int pos) {
        return quint32(quint8(header[pos])) | (quint32(quint8(header[pos + 1])) << 8)
            | (quint32(quint8(header[pos + 2])) << 16)
            | (quint32(quint8(header[pos + 3])) << 24);
    };

    int offset = 12;
    int dataSize = 0;
    bool hasFormat = false;
    while (offset + 8 <= header.size()) {
        const QByteArray chunkId = header.mid(offset, 4);
        const quint32 chunkSize = read32(offset + 4);
        const int body = offset + 8;
        if (chunkId == "fmt " && body + 16 <= header.size()) {
            info.channels = read16(body + 2);
            info.sampleRate = int(read32(body + 4));
            info.bits = read16(body + 14);
            hasFormat = true;
        } else if (chunkId == "data") {
            const qint64 available = file.size() - body;
            dataSize = int(chunkSize == 0 ? available
                                          : qMin<qint64>(chunkSize, available));
            break;
        }
        offset = body + int(chunkSize) + (int(chunkSize) & 1);
    }

    if (hasFormat && info.sampleRate > 0 && info.channels > 0 && info.bits > 0 && dataSize > 0) {
        const qint64 bytesPerSecond = qint64(info.sampleRate) * info.channels * info.bits / 8;
        if (bytesPerSecond > 0)
            info.durationMs = qint64(dataSize) * 1000 / bytesPerSecond;
    }
    return info;
}

QJsonArray peaksToJson(const QVector<float> &peaks)
{
    QJsonArray array;
    for (const float value : peaks)
        array.append(double(qBound(0.0f, value, 1.0f)));
    return array;
}

QVector<float> peaksFromJson(const QJsonArray &array)
{
    QVector<float> peaks;
    peaks.reserve(array.size());
    for (const QJsonValue &value : array)
        peaks.append(float(value.toDouble()));
    return peaks;
}

} // namespace

RecordingStore::RecordingStore(QObject *parent)
    : QObject(parent)
{
}

QString RecordingStore::directory() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/dvoice-qt/recordings");
}

bool RecordingStore::ensureDirectory(QString *error)
{
    QDir dir;
    if (dir.mkpath(directory()))
        return true;
    if (error)
        *error = QStringLiteral("无法创建录音库目录：%1").arg(directory());
    return false;
}

QString RecordingStore::metadataPath(const QString &id) const
{
    return directory() + QLatin1Char('/') + id + QStringLiteral(".json");
}

QString RecordingStore::uniqueBase(const QString &suffix) const
{
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QString base = QStringLiteral("rec-%1").arg(stamp);
    int index = 1;
    while (QFileInfo::exists(directory() + QLatin1Char('/') + base + suffix)) {
        base = QStringLiteral("rec-%1-%2").arg(stamp).arg(index++);
    }
    return base;
}

QString RecordingStore::nextTitle() const
{
    int maximum = 0;
    const QRegularExpression pattern(QStringLiteral("^录音\\s*(\\d+)$"));
    for (const Recording &recording : list()) {
        const QRegularExpressionMatch match = pattern.match(recording.title);
        if (match.hasMatch())
            maximum = qMax(maximum, match.captured(1).toInt());
    }
    return QStringLiteral("录音 %1").arg(maximum + 1);
}

QVector<Recording> RecordingStore::list(QString *error) const
{
    QVector<Recording> recordings;
    QDir dir(directory());
    if (!dir.exists())
        return recordings;

    const QStringList files = dir.entryList(
        {QStringLiteral("*.wav"), QStringLiteral("*.mp3"), QStringLiteral("*.flac"),
         QStringLiteral("*.ogg"), QStringLiteral("*.opus"), QStringLiteral("*.m4a"),
         QStringLiteral("*.aac"), QStringLiteral("*.wma")},
        QDir::Files, QDir::Name);
    for (const QString &file : files) {
        const QFileInfo info(dir.filePath(file));
        const QString id = info.completeBaseName();
        Recording recording;
        recording.id = id;
        recording.path = info.absoluteFilePath();
        recording.title = id;
        recording.createdAt = info.lastModified();
        recording.sampleRate = 0;
        recording.channels = 1;
        recording.bits = 16;

        QFile metadata(metadataPath(id));
        if (metadata.open(QIODevice::ReadOnly)) {
            const QJsonDocument doc = QJsonDocument::fromJson(metadata.readAll());
            if (doc.isObject()) {
                const QJsonObject object = doc.object();
                recording.title = object.value(QStringLiteral("title")).toString(recording.title);
                recording.transcript = object.value(QStringLiteral("transcript")).toString();
                recording.durationMs = qint64(object.value(QStringLiteral("durationMs")).toDouble());
                recording.sampleRate = object.value(QStringLiteral("sampleRate")).toInt();
                recording.channels = object.value(QStringLiteral("channels")).toInt(1);
                recording.bits = object.value(QStringLiteral("bits")).toInt(16);
                const QDateTime created = QDateTime::fromString(
                    object.value(QStringLiteral("created")).toString(), Qt::ISODate);
                if (created.isValid())
                    recording.createdAt = created;
                recording.peaks = peaksFromJson(
                    object.value(QStringLiteral("peaks")).toArray());
            }
        }

        if (recording.durationMs <= 0 || recording.sampleRate <= 0) {
            const WavInfo parsed = parseWavInfo(recording.path);
            if (recording.durationMs <= 0)
                recording.durationMs = parsed.durationMs;
            if (recording.sampleRate <= 0)
                recording.sampleRate = parsed.sampleRate;
            if (recording.channels <= 0)
                recording.channels = parsed.channels;
            if (recording.bits <= 0)
                recording.bits = parsed.bits;
        }
        recordings.append(recording);
    }

    std::sort(recordings.begin(), recordings.end(),
              [](const Recording &a, const Recording &b) {
                  return a.createdAt > b.createdAt;
              });
    Q_UNUSED(error)
    return recordings;
}

QString RecordingStore::addWav(const QByteArray &wav, const RecordingInfo &info, QString *error)
{
    if (!ensureDirectory(error))
        return {};
    const QString base = uniqueBase(QStringLiteral(".wav"));
    const QString path = directory() + QLatin1Char('/') + base + QStringLiteral(".wav");

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(wav) != wav.size() || !file.commit()) {
        if (error)
            *error = QStringLiteral("无法写入录音：%1").arg(path);
        return {};
    }

    Recording recording;
    recording.id = base;
    recording.path = path;
    recording.title = info.title.isEmpty() ? nextTitle() : info.title;
    recording.createdAt = info.createdAt.isValid() ? info.createdAt : QDateTime::currentDateTime();
    recording.transcript = info.transcript;
    recording.durationMs = info.durationMs;
    recording.sampleRate = info.sampleRate;
    recording.channels = info.channels;
    recording.bits = info.bits;
    recording.peaks = info.peaks;

    if (!writeMetadata(base, recording, error)) {
        QFile::remove(path);
        return {};
    }
    emit changed();
    return base;
}

QString RecordingStore::addFile(const QString &sourcePath, const RecordingInfo &info, QString *error)
{
    if (!ensureDirectory(error))
        return {};
    const QFileInfo source(sourcePath);
    const QString suffix = QStringLiteral(".") + source.suffix().toLower();
    const QString base = uniqueBase(suffix);
    const QString path = directory() + QLatin1Char('/') + base + suffix;
    if (!QFile::copy(source.absoluteFilePath(), path)) {
        if (error)
            *error = QStringLiteral("无法复制音频：%1").arg(sourcePath);
        return {};
    }

    Recording recording;
    recording.id = base;
    recording.path = path;
    recording.title = info.title.isEmpty() ? nextTitle() : info.title;
    recording.createdAt = info.createdAt.isValid() ? info.createdAt : QDateTime::currentDateTime();
    recording.transcript = info.transcript;
    recording.durationMs = info.durationMs;
    recording.sampleRate = info.sampleRate;
    recording.channels = info.channels;
    recording.bits = info.bits;
    recording.peaks = info.peaks;

    if (!writeMetadata(base, recording, error)) {
        QFile::remove(path);
        return {};
    }
    emit changed();
    return base;
}

bool RecordingStore::writeMetadata(const QString &id, const Recording &recording, QString *error) const
{
    QJsonObject object;
    object.insert(QStringLiteral("title"), recording.title);
    object.insert(QStringLiteral("created"), recording.createdAt.toString(Qt::ISODate));
    object.insert(QStringLiteral("durationMs"), double(recording.durationMs));
    object.insert(QStringLiteral("sampleRate"), recording.sampleRate);
    object.insert(QStringLiteral("channels"), recording.channels);
    object.insert(QStringLiteral("bits"), recording.bits);
    object.insert(QStringLiteral("transcript"), recording.transcript);
    object.insert(QStringLiteral("peaks"), peaksToJson(recording.peaks));

    QSaveFile file(metadataPath(id));
    if (!file.open(QIODevice::WriteOnly)
        || file.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) < 0
        || !file.commit()) {
        if (error)
            *error = QStringLiteral("无法写入录音元数据");
        return false;
    }
    return true;
}

bool RecordingStore::update(const Recording &recording, QString *error)
{
    const bool ok = writeMetadata(recording.id, recording, error);
    if (ok)
        emit changed();
    return ok;
}

bool RecordingStore::remove(const Recording &recording, QString *error)
{
    bool ok = QFile::remove(recording.path);
    QFile::remove(metadataPath(recording.id));
    if (!ok && error)
        *error = QStringLiteral("无法删除录音文件");
    if (ok)
        emit changed();
    return ok;
}
