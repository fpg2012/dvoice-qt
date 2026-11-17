// 波形 + 频谱显示：接收 16-bit 单声道 PCM，滚动显示最近 2 秒波形和实时频谱。
#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QWidget>

class QTimer;

class AudioMonitor : public QWidget
{
    Q_OBJECT

public:
    explicit AudioMonitor(QWidget *parent = nullptr);

    void setSampleRate(int sampleRate);
    void setCaption(const QString &caption);
    void feed(const QByteArray &pcm);
    void clear();

    QSize sizeHint() const override { return {640, 190}; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void scheduleRepaint();
    void computeSpectrum();

    QVector<float> m_samples;
    QVector<float> m_bins;
    QByteArray m_remainder;
    QTimer *m_repaint = nullptr;
    QString m_caption;
    int m_rate = 16000;
    bool m_dirty = false;
};
