// 波形 + 频谱显示：接收 16-bit 单声道 PCM。
// 录音用瞬时频谱（柱状），播放用滚动声谱图（时间 × 频率热力图）。
#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QVector>
#include <QWidget>

class QTimer;

class AudioMonitor : public QWidget
{
    Q_OBJECT

public:
    enum class Mode {
        Bars,        // 瞬时频谱：横轴频率，柱高能量
        Spectrogram, // 声谱图：横轴时间，纵轴频率，颜色能量
    };

    explicit AudioMonitor(QWidget *parent = nullptr);

    void setSampleRate(int sampleRate);
    void setMode(Mode mode);
    Mode mode() const { return m_mode; }
    void setCaption(const QString &caption);
    void feed(const QByteArray &pcm);
    void clear();

    QSize sizeHint() const override { return {640, 210}; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void scheduleRepaint();
    void computeSpectrum();
    void processSpectrogram();
    void pushSpectrogramColumn();
    void resetSpectrogram();

    QVector<float> m_samples;
    QVector<float> m_bins;
    QByteArray m_remainder;
    QTimer *m_repaint = nullptr;
    QString m_caption;
    int m_rate = 16000;
    Mode m_mode = Mode::Bars;
    bool m_dirty = false;

    QImage m_waterfall;
    QVector<float> m_specWindow;
    QVector<float> m_specPending;
    int m_specColumns = 0;
};
