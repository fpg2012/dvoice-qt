// 录音库：统一存放 WAV/音频文件与元数据（标题、时间、时长、波形峰值、转写）。
#pragma once

#include <QAudioFormat>
#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

struct Recording {
    QString id;
    QString path;
    QString title;
    QString transcript;
    QDateTime createdAt;
    qint64 durationMs = 0;
    int sampleRate = 0;
    int channels = 1;
    int bits = 16;
    QVector<float> peaks;
};

struct RecordingInfo {
    QString title;
    QDateTime createdAt;
    QString transcript;
    qint64 durationMs = 0;
    int sampleRate = 0;
    int channels = 1;
    int bits = 16;
    QVector<float> peaks;
};

class RecordingStore : public QObject
{
    Q_OBJECT

public:
    explicit RecordingStore(QObject *parent = nullptr);

    QString directory() const;
    bool ensureDirectory(QString *error = nullptr);

    QVector<Recording> list(QString *error = nullptr) const;
    QString nextTitle() const;

    QString addWav(const QByteArray &wav, const RecordingInfo &info, QString *error);
    QString addFile(const QString &sourcePath, const RecordingInfo &info, QString *error);
    bool update(const Recording &recording, QString *error = nullptr);
    bool remove(const Recording &recording, QString *error);

signals:
    void changed();

private:
    QString metadataPath(const QString &id) const;
    bool writeMetadata(const QString &id, const Recording &recording, QString *error) const;
    QString uniqueBase(const QString &suffix) const;
};
