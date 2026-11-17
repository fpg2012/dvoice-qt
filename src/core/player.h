// 朗读播放器：把 TTS 返回的 PCM 边收边播，完整收到后支持拖动定位。
#pragma once

#include "core/tts.h"

#include <QAudioDevice>
#include <QByteArray>
#include <QObject>
#include <QStringList>

class QAudioSink;
class QBuffer;
class QIODevice;
class QTimer;

class Player : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Idle,
        Buffering,
        Playing,
        Paused,
        Finished,
        Failed,
    };
    Q_ENUM(State)

    explicit Player(QObject *parent = nullptr);
    ~Player() override;

    void setOutputDevice(const QAudioDevice &device);
    QAudioDevice outputDevice() const { return m_device; }

    void setVolume(qreal volume);
    qreal volume() const { return m_volume; }

    void play(const QString &text, const tts::Request &request);
    void stop();
    void togglePause();
    void seek(qint64 ms);

    State state() const { return m_state; }
    bool hasAudio() const { return !m_pcm.isEmpty(); }
    bool canSeek() const;
    qint64 positionMs() const;
    qint64 durationMs() const { return m_durationMs; }
    qint64 bufferedMs() const;

signals:
    void stateChanged(Player::State state);
    void positionChanged(qint64 position, qint64 duration, qint64 buffered);
    void audioChunk(const QByteArray &pcm);
    void errorOccurred(const QString &message);
    void finished();

private:
    void startChunk();
    void onAudio(const QByteArray &data);
    void onChunkFinished();
    void onFailed(const QString &message);
    void fail(const QString &message);
    void ensureSink();
    void flushWrites();
    void checkDone();
    void finishPlayback();
    void stopSink();
    void setState(State state);
    void tick();

    QAudioDevice m_device;
    qreal m_volume = 0.8;
    tts::Client m_client;
    tts::Request m_request;
    QStringList m_chunks;
    int m_chunkIndex = 0;

    QByteArray m_pcm;
    QByteArray m_writeBuffer;
    qint64 m_written = 0;

    QAudioSink *m_sink = nullptr;
    QIODevice *m_sinkIo = nullptr;
    QBuffer *m_buffer = nullptr;
    QTimer *m_timer = nullptr;

    State m_state = State::Idle;
    bool m_streaming = true;
    bool m_networkDone = false;
    bool m_started = false;
    bool m_stopping = false;
    qint64 m_baseMs = 0;
    qint64 m_frozenPosition = 0;
    qint64 m_durationMs = 0;
};
