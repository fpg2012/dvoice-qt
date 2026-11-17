#include "core/player.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QBuffer>
#include <QTimer>

namespace {
constexpr int kSampleRate = 24000;
constexpr int kChannels = 1;
constexpr int kBytesPerMs = kSampleRate * kChannels * 2 / 1000; // 48

QAudioFormat pcmFormat()
{
    QAudioFormat format;
    format.setSampleRate(kSampleRate);
    format.setChannelCount(kChannels);
    format.setSampleFormat(QAudioFormat::Int16);
    return format;
}
} // namespace

Player::Player(QObject *parent)
    : QObject(parent)
    , m_timer(new QTimer(this))
{
    m_timer->setInterval(100);
    connect(m_timer, &QTimer::timeout, this, &Player::tick);
    connect(&m_client, &tts::Client::audioChunk, this, &Player::onAudio);
    connect(&m_client, &tts::Client::finished, this, &Player::onChunkFinished);
    connect(&m_client, &tts::Client::failed, this, &Player::onFailed);
}

Player::~Player()
{
    m_stopping = true;
    m_client.abort();
    stopSink();
}

void Player::setOutputDevice(const QAudioDevice &device)
{
    m_device = device;
}

void Player::setVolume(qreal volume)
{
    m_volume = qBound(0.0, volume, 1.0);
    if (m_sink)
        m_sink->setVolume(m_volume);
}

bool Player::canSeek() const
{
    return m_networkDone && !m_pcm.isEmpty();
}

qint64 Player::bufferedMs() const
{
    return m_pcm.size() / kBytesPerMs;
}

qint64 Player::positionMs() const
{
    if (m_state == State::Finished)
        return m_durationMs;
    if (m_state == State::Paused || m_state == State::Buffering || m_state == State::Failed)
        return m_state == State::Paused ? m_frozenPosition : 0;
    if (!m_sink || !m_started)
        return 0;
    const qint64 processed = m_sink->processedUSecs() / 1000;
    qint64 position = m_baseMs + processed;
    if (m_durationMs > 0)
        position = qMin(position, m_durationMs);
    return qMax<qint64>(0, position);
}

void Player::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged(state);
}

void Player::play(const QString &text, const tts::Request &request)
{
    stop();
    if (request.apiKey.trimmed().isEmpty()) {
        setState(State::Failed);
        emit errorOccurred(QStringLiteral("未设置 API Key"));
        return;
    }

    m_chunks = tts::splitText(text, 8000);
    if (m_chunks.isEmpty()) {
        setState(State::Failed);
        emit errorOccurred(QStringLiteral("没有可朗读的文本"));
        return;
    }

    m_request = request;
    m_chunkIndex = 0;
    m_networkDone = false;
    m_durationMs = 0;
    m_baseMs = 0;
    m_frozenPosition = 0;
    m_written = 0;
    m_pcm.clear();
    m_writeBuffer.clear();
    m_streaming = true;
    m_started = false;
    setState(State::Buffering);
    m_timer->start();
    startChunk();
}

void Player::startChunk()
{
    if (m_chunkIndex >= m_chunks.size()) {
        m_networkDone = true;
        m_durationMs = m_pcm.size() / kBytesPerMs;
        checkDone();
        return;
    }
    tts::Request request = m_request;
    request.text = m_chunks.at(m_chunkIndex++);
    m_client.start(request);
}

void Player::onAudio(const QByteArray &data)
{
    if (m_stopping || data.isEmpty())
        return;

    m_pcm.append(data);
    if (!m_networkDone)
        m_durationMs = 0;
    m_writeBuffer.append(data);
    emit audioChunk(data);

    if (!m_sink)
        ensureSink();
    flushWrites();

    if (m_state == State::Buffering && m_sink)
        setState(State::Playing);
}

void Player::onChunkFinished()
{
    if (m_stopping)
        return;
    if (m_chunkIndex < m_chunks.size()) {
        startChunk();
        return;
    }
    m_networkDone = true;
    m_durationMs = m_pcm.size() / kBytesPerMs;
    if (m_pcm.isEmpty()) {
        setState(State::Failed);
        emit errorOccurred(QStringLiteral("服务未返回音频"));
        m_timer->stop();
        stopSink();
        return;
    }
    checkDone();
}

void Player::onFailed(const QString &message)
{
    if (m_stopping)
        return;
    fail(message);
}

void Player::fail(const QString &message)
{
    m_timer->stop();
    m_client.abort();
    m_stopping = true;
    stopSink();
    m_stopping = false;
    m_networkDone = false;
    setState(State::Failed);
    emit errorOccurred(message);
}

void Player::ensureSink()
{
    QAudioFormat format = pcmFormat();
    m_sink = m_device.isNull() ? new QAudioSink(format, this)
                               : new QAudioSink(m_device, format, this);
    m_sink->setVolume(m_volume);
    connect(m_sink, &QAudioSink::stateChanged, this, [this](QAudio::State state) {
        if (m_stopping)
            return;
        if (state == QAudio::StoppedState && m_sink->error() != QAudio::NoError) {
            fail(QStringLiteral("播放设备错误：%1").arg(m_device.isNull()
                                                            ? QStringLiteral("系统默认")
                                                            : m_device.description()));
            return;
        }
        if (state == QAudio::ActiveState)
            flushWrites();
        if (state == QAudio::IdleState || state == QAudio::StoppedState) {
            if (!m_streaming && m_buffer && m_buffer->atEnd()) {
                finishPlayback();
                return;
            }
            checkDone();
        }
    });

    m_sinkIo = m_sink->start();
    m_started = m_sinkIo != nullptr;
    if (!m_started) {
        fail(QStringLiteral("无法打开扬声器"));
    }
}

void Player::flushWrites()
{
    if (!m_streaming || !m_sinkIo)
        return;
    while (!m_writeBuffer.isEmpty()) {
        const qint64 accepted = m_sinkIo->write(m_writeBuffer);
        if (accepted <= 0)
            break;
        m_written += accepted;
        m_writeBuffer.remove(0, int(accepted));
    }
}

void Player::checkDone()
{
    if (!m_streaming || !m_networkDone || !m_started || !m_sink)
        return;
    if (!m_writeBuffer.isEmpty() || m_written < m_pcm.size())
        return;
    if (m_sink->state() == QAudio::IdleState || m_sink->state() == QAudio::StoppedState)
        finishPlayback();
}

void Player::finishPlayback()
{
    if (m_state == State::Finished)
        return;
    m_durationMs = m_pcm.size() / kBytesPerMs;
    m_frozenPosition = m_durationMs;
    stopSink();
    m_timer->stop();
    setState(State::Finished);
    emit positionChanged(m_durationMs, m_durationMs, m_durationMs);
    emit finished();
}

void Player::stopSink()
{
    if (m_sink) {
        m_sink->disconnect(this);
        m_sink->stop();
        m_sink->deleteLater();
        m_sink = nullptr;
    }
    m_sinkIo = nullptr;
    if (m_buffer) {
        m_buffer->close();
        m_buffer->deleteLater();
        m_buffer = nullptr;
    }
    m_started = false;
}

void Player::stop()
{
    if (m_state == State::Idle && m_pcm.isEmpty())
        return;
    m_stopping = true;
    m_client.abort();
    m_timer->stop();
    stopSink();
    m_chunks.clear();
    m_chunkIndex = 0;
    m_pcm.clear();
    m_writeBuffer.clear();
    m_written = 0;
    m_networkDone = false;
    m_streaming = true;
    m_baseMs = 0;
    m_frozenPosition = 0;
    m_durationMs = 0;
    m_state = State::Idle;
    m_stopping = false;
    emit stateChanged(State::Idle);
    emit positionChanged(0, 0, 0);
}

void Player::togglePause()
{
    if (!m_sink)
        return;
    if (m_state == State::Playing) {
        m_frozenPosition = positionMs();
        m_sink->suspend();
        setState(State::Paused);
    } else if (m_state == State::Paused) {
        m_baseMs = m_frozenPosition - m_sink->processedUSecs() / 1000;
        m_sink->resume();
        setState(State::Playing);
    }
}

void Player::seek(qint64 ms)
{
    if (!canSeek())
        return;
    const qint64 target = qBound<qint64>(0, ms, m_durationMs);
    stopSink();

    m_streaming = false;
    m_baseMs = target;
    m_frozenPosition = target;
    m_buffer = new QBuffer(&m_pcm, this);
    m_buffer->open(QIODevice::ReadOnly);
    m_buffer->seek(target * kBytesPerMs);

    QAudioFormat format = pcmFormat();
    m_sink = m_device.isNull() ? new QAudioSink(format, this)
                               : new QAudioSink(m_device, format, this);
    m_sink->setVolume(m_volume);
    connect(m_sink, &QAudioSink::stateChanged, this, [this](QAudio::State state) {
        if (m_stopping)
            return;
        if (state == QAudio::StoppedState && m_sink->error() != QAudio::NoError) {
            fail(QStringLiteral("播放设备错误：%1").arg(m_device.isNull()
                                                            ? QStringLiteral("系统默认")
                                                            : m_device.description()));
            return;
        }
        if (!m_streaming && state == QAudio::IdleState && m_buffer && m_buffer->atEnd())
            finishPlayback();
    });
    m_sink->start(m_buffer);
    m_started = true;
    setState(State::Playing);
    m_timer->start();
    emit positionChanged(target, m_durationMs, m_durationMs);
}

void Player::tick()
{
    if (m_stopping)
        return;
    if (m_state == State::Playing || m_state == State::Paused || m_state == State::Buffering) {
        if (m_streaming)
            flushWrites();
        emit positionChanged(positionMs(), m_durationMs, bufferedMs());
        if (m_streaming)
            checkDone();
        else if (m_sink && m_sink->state() == QAudio::IdleState && m_buffer && m_buffer->atEnd())
            finishPlayback();
    }
}
