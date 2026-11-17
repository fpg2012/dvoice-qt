#include "core/tts.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace tts {

namespace {

bool isHan(QChar c)
{
    const char32_t u = c.unicode();
    return (u >= 0x3400 && u <= 0x4DBF) || (u >= 0x4E00 && u <= 0x9FFF)
        || (u >= 0xF900 && u <= 0xFAFF) || (u >= 0x20000 && u <= 0x2FA1F);
}

QString friendlyStatus(int status)
{
    switch (status) {
    case 400:
        return QStringLiteral("文本为空或未通过内容审核");
    case 429:
        return QStringLiteral("并发请求过多（每账号上限 50）");
    case 503:
        return QStringLiteral("服务繁忙，请稍后重试");
    case 504:
        return QStringLiteral("合成超时，请稍后重试");
    default:
        return QStringLiteral("合成失败（HTTP %1）").arg(status);
    }
}

void flushUnit(QStringList &units, QString &unit)
{
    const QString trimmed = unit.trimmed();
    if (!trimmed.isEmpty())
        units.append(trimmed);
    unit.clear();
}

QStringList hardSplit(const QString &text, int maxBilled)
{
    QStringList parts;
    QString current;
    int billed = 0;
    for (const QChar c : text) {
        const int cost = isHan(c) ? 2 : 1;
        if (billed + cost > maxBilled && !current.isEmpty()) {
            parts.append(current);
            current.clear();
            billed = 0;
        }
        current.append(c);
        billed += cost;
    }
    if (!current.trimmed().isEmpty())
        parts.append(current);
    return parts;
}

} // namespace

int billedChars(const QString &text)
{
    int total = 0;
    for (const QChar c : text)
        total += isHan(c) ? 2 : 1;
    return total;
}

QStringList splitText(const QString &text, int maxBilled)
{
    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    normalized = normalized.trimmed();
    if (normalized.isEmpty())
        return {};

    // 先按句末标点和换行切成短句，再往请求里打包。
    QStringList units;
    QString unit;
    const QString stops = QStringLiteral("。！？!?；;\n");
    for (const QChar c : normalized) {
        unit.append(c);
        if (stops.contains(c)) {
            flushUnit(units, unit);
            continue;
        }
        if (billedChars(unit) > maxBilled) {
            flushUnit(units, unit);
        }
    }
    flushUnit(units, unit);

    QStringList result;
    QString current;
    int currentBilled = 0;
    auto flush = [&] {
        const QString trimmed = current.trimmed();
        if (!trimmed.isEmpty())
            result.append(trimmed);
        current.clear();
        currentBilled = 0;
    };

    for (const QString &piece : units) {
        const int cost = billedChars(piece);
        if (cost > maxBilled) {
            flush();
            result.append(hardSplit(piece, maxBilled));
            continue;
        }
        if (currentBilled + cost > maxBilled)
            flush();
        if (!current.isEmpty())
            current.append(QLatin1Char(' '));
        current.append(piece);
        currentBilled += cost;
    }
    flush();
    return result;
}

QString errorMessage(int status, const QByteArray &body)
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

void Client::abort()
{
    if (!m_reply)
        return;
    m_aborted = true;
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
}

void Client::start(const Request &request)
{
    abort();
    m_aborted = false;
    m_sawAudio = false;
    m_errorBody.clear();

    if (request.apiKey.trimmed().isEmpty()) {
        emit failed(QStringLiteral("未设置 API Key"));
        return;
    }
    if (request.text.trimmed().isEmpty()) {
        emit failed(QStringLiteral("没有可朗读的文本"));
        return;
    }

    QJsonObject body;
    body.insert(QStringLiteral("text"), request.text);
    body.insert(QStringLiteral("voice_id"),
                request.voiceId == QLatin1String("tide") ? QStringLiteral("tide")
                                                         : QStringLiteral("stella"));
    body.insert(QStringLiteral("format"), QStringLiteral("pcm"));
    body.insert(QStringLiteral("filter_markdown"), request.filterMarkdown);

    QString base = request.baseUrl.trimmed();
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);

    QNetworkRequest http(QUrl(base + QStringLiteral("/v1/tools/tts")));
    http.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    http.setRawHeader("Authorization", "Bearer " + request.apiKey.trimmed().toUtf8());
    http.setTransferTimeout(90000);

    m_reply = m_net->post(http, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_reply, &QNetworkReply::readyRead, this, &Client::handleReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &Client::handleFinished);
}

void Client::handleReadyRead()
{
    if (!m_reply)
        return;
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray data = m_reply->readAll();
    if (data.isEmpty())
        return;
    if (status >= 400) {
        m_errorBody.append(data);
        return;
    }
    m_sawAudio = true;
    emit audioChunk(data);
}

void Client::handleFinished()
{
    if (!m_reply)
        return;
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (reply->error() != QNetworkReply::NoError) {
        if (status >= 400) {
            m_errorBody.append(reply->readAll());
            const QString message = errorMessage(status, m_errorBody);
            reply->deleteLater();
            emit failed(message);
            return;
        }
        const QString message = m_sawAudio
            ? QStringLiteral("音频流中断：%1").arg(reply->errorString())
            : reply->errorString();
        reply->deleteLater();
        emit failed(message);
        return;
    }

    if (status >= 400) {
        m_errorBody.append(reply->readAll());
        const QString message = errorMessage(status, m_errorBody);
        reply->deleteLater();
        emit failed(message);
        return;
    }

    if (!m_sawAudio) {
        reply->deleteLater();
        emit failed(QStringLiteral("服务未返回音频"));
        return;
    }

    reply->deleteLater();
    if (!m_aborted)
        emit finished();
}

} // namespace tts
