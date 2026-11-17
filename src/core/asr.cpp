#include "core/asr.h"

#include "core/wav.h"

#include <QHttpMultiPart>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace asr {

namespace {

QString friendlyStatus(int status)
{
    switch (status) {
    case 400:
        return QStringLiteral("音频格式无效或超过 120 秒");
    case 408:
        return QStringLiteral("上传超时（需在 10 分钟内完成）");
    case 413:
        return QStringLiteral("音频超过 8 MiB 上限");
    case 429:
        return QStringLiteral("并发请求过多（每账号上限 50）");
    case 503:
        return QStringLiteral("服务繁忙，请稍后重试");
    case 504:
        return QStringLiteral("识别超时，请稍后重试");
    default:
        return QStringLiteral("识别失败（HTTP %1）").arg(status);
    }
}

} // namespace

QString httpErrorMessage(int status, const QByteArray &body)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
        const QJsonObject error = doc.object().value(QStringLiteral("error")).toObject();
        const QString message = error.value(QStringLiteral("message")).toString();
        if (!message.isEmpty())
            return message;
    }
    return friendlyStatus(status);
}

Client::Client(QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
{
}

void Client::setEndpoint(const QString &baseUrl, const QString &apiKey)
{
    m_baseUrl = baseUrl;
    while (m_baseUrl.endsWith(QLatin1Char('/')))
        m_baseUrl.chop(1);
    m_apiKey = apiKey;
}

void Client::abort()
{
    if (!m_reply)
        return;
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
}

void Client::transcribe(const QByteArray &wav)
{
    abort();
    if (m_apiKey.isEmpty()) {
        Result result;
        result.error = QStringLiteral("未设置 API Key");
        QTimer::singleShot(0, this, [this, result] { emit finished(result); });
        return;
    }

    auto *multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart filePart;
    filePart.setRawHeader("Content-Disposition", "form-data; name=\"file\"; filename=\"audio.wav\"");
    filePart.setRawHeader("Content-Type", "audio/wav");
    filePart.setBody(wav);
    multi->append(filePart);

    QNetworkRequest request(QUrl(m_baseUrl + QStringLiteral("/v1/tools/asr")));
    request.setRawHeader("Authorization", "Bearer " + m_apiKey.toUtf8());
    request.setTransferTimeout(150000);

    m_reply = m_net->post(request, multi);
    multi->setParent(m_reply);
    connect(m_reply, &QNetworkReply::finished, this, &Client::handleReply);
}

void Client::handleReply()
{
    if (!m_reply)
        return;
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;

    Result result;
    result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();

    if (reply->error() != QNetworkReply::NoError) {
        result.error = result.status > 0 ? httpErrorMessage(result.status, body)
                                         : reply->errorString();
    } else {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            result.error = QStringLiteral("无法解析识别结果");
        } else {
            const QJsonObject object = doc.object();
            result.text = object.value(QStringLiteral("text")).toString();
            result.requestId = object.value(QStringLiteral("request_id")).toString();
            result.durationMs = object.value(QStringLiteral("duration_ms")).toInt();
        }
    }

    reply->deleteLater();
    emit finished(result);
}

Queue::Queue(QObject *parent)
    : QObject(parent)
{
    connect(&m_client, &Client::finished, this, &Queue::onFinished);
}

void Queue::configure(const QString &baseUrl, const QString &apiKey)
{
    m_client.setEndpoint(baseUrl, apiKey);
}

void Queue::enqueue(const QByteArray &pcm, int sampleRate, int startMs, int endMs)
{
    m_pcm.append(pcm);
    m_sampleRates.append(sampleRate);
    m_starts.append(startMs);
    m_ends.append(endMs);
    pump();
}

int Queue::pending() const
{
    return int(m_pcm.size()) + (m_currentIndex >= 0 ? 1 : 0);
}

void Queue::cancel()
{
    ++m_epoch;
    m_pcm.clear();
    m_sampleRates.clear();
    m_starts.clear();
    m_ends.clear();
    m_currentIndex = -1;
    m_retries = 0;
    m_running = false;
    m_client.abort();
    emit idle();
}

void Queue::pump()
{
    if (m_currentIndex >= 0)
        return;
    if (m_pcm.isEmpty()) {
        if (m_running) {
            m_running = false;
            emit idle();
        }
        return;
    }

    m_currentIndex = m_nextIndex++;
    m_retries = 0;
    m_running = true;
    const QByteArray pcm = m_pcm.first();
    const int sampleRate = m_sampleRates.first();
    const int startMs = m_starts.first();
    emit started(m_currentIndex, startMs);
    m_client.transcribe(wavFromPcm(pcm, sampleRate));
}

void Queue::retryLater(int epoch)
{
    QTimer::singleShot(1500 + 1000 * m_retries, this, [this, epoch] {
        if (epoch != m_epoch || m_currentIndex < 0 || m_pcm.isEmpty())
            return;
        m_client.transcribe(wavFromPcm(m_pcm.first(), m_sampleRates.first()));
    });
}

void Queue::onFinished(const asr::Result &result)
{
    if (m_currentIndex < 0 || m_pcm.isEmpty())
        return;

    const int index = m_currentIndex;
    const int startMs = m_starts.first();
    const int endMs = m_ends.first();
    const bool retryable = result.status == 0 || result.status == 408 || result.status == 429
        || result.status == 503 || result.status == 504;

    if (result.ok()) {
        const QByteArray pcm = m_pcm.takeFirst();
        Q_UNUSED(pcm)
        m_sampleRates.removeFirst();
        m_starts.removeFirst();
        m_ends.removeFirst();
        m_currentIndex = -1;
        m_retries = 0;
        emit recognized(index, result.text, result.durationMs, startMs, endMs);
        pump();
        return;
    }

    if (retryable && m_retries < 2) {
        ++m_retries;
        const int epoch = m_epoch;
        retryLater(epoch);
        return;
    }

    m_pcm.removeFirst();
    m_sampleRates.removeFirst();
    m_starts.removeFirst();
    m_ends.removeFirst();
    m_currentIndex = -1;
    m_retries = 0;
    emit failed(index, result.error.isEmpty() ? QStringLiteral("识别失败") : result.error);
    pump();
}

} // namespace asr
