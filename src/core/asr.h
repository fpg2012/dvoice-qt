// DeepSeek 语音识别：一次请求上传完整音频，返回整段文本（服务端不支持流式）。
#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVector>

class QNetworkAccessManager;
class QNetworkReply;

namespace asr {

struct Result {
    QString text;
    QString requestId;
    int durationMs = 0;
    int status = 0;
    QString error;

    bool ok() const { return error.isEmpty() && status == 200; }
};

class Client : public QObject
{
    Q_OBJECT

public:
    explicit Client(QObject *parent = nullptr);

    void setEndpoint(const QString &baseUrl, const QString &apiKey);
    void transcribe(const QByteArray &wav);
    void abort();
    bool busy() const { return m_reply != nullptr; }

signals:
    void finished(asr::Result result);

private:
    void handleReply();

    QNetworkAccessManager *m_net;
    QNetworkReply *m_reply = nullptr;
    QString m_baseUrl;
    QString m_apiKey;
};

// 顺序识别队列：保证文本按录音顺序上屏；429/503/504/网络错误自动重试两次。
class Queue : public QObject
{
    Q_OBJECT

public:
    explicit Queue(QObject *parent = nullptr);

    void configure(const QString &baseUrl, const QString &apiKey);
    void enqueue(const QByteArray &pcm, int sampleRate, int startMs, int endMs);
    void cancel();

    bool busy() const { return m_running; }
    int pending() const;

signals:
    void started(int index, int startMs);
    void recognized(int index, QString text, int durationMs, int startMs, int endMs);
    void failed(int index, QString message);
    void idle();

private:
    void pump();
    void onFinished(const asr::Result &result);
    void retryLater(int epoch);

    Client m_client;
    QVector<QByteArray> m_pcm;
    QVector<int> m_sampleRates;
    QVector<int> m_starts;
    QVector<int> m_ends;
    int m_nextIndex = 0;
    int m_currentIndex = -1;
    int m_retries = 0;
    int m_epoch = 0;
    bool m_running = false;
};

QString httpErrorMessage(int status, const QByteArray &body);

} // namespace asr
