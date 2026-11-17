// DeepSeek 语音合成：HTTP 响应体是边合成边返回的 24 kHz / 16-bit / 单声道 PCM。
#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;

namespace tts {

struct Request {
    QString text;
    QString voiceId = QStringLiteral("stella");
    QString baseUrl = QStringLiteral("https://api.deepseek.com");
    QString apiKey;
    bool filterMarkdown = true;
};

class Client : public QObject
{
    Q_OBJECT

public:
    explicit Client(QObject *parent = nullptr);

    void start(const Request &request);
    void abort();
    bool busy() const { return m_reply != nullptr; }

signals:
    void audioChunk(const QByteArray &pcm);
    void finished();
    void failed(const QString &message);

private:
    void handleReadyRead();
    void handleFinished();

    QNetworkAccessManager *m_net;
    QNetworkReply *m_reply = nullptr;
    QByteArray m_errorBody;
    bool m_aborted = false;
    bool m_sawAudio = false;
};

// 汉字按 2 个计费字符、其他字符按 1 个计算。
int billedChars(const QString &text);

// 按句切分并打包成每次不超过 maxBilled 个计费字符的请求。
QStringList splitText(const QString &text, int maxBilled = 8000);

QString errorMessage(int status, const QByteArray &body);

} // namespace tts
