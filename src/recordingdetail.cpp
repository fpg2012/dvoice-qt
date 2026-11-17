#include "recordingdetail.h"

#include "core/audiofile.h"
#include "core/audiodevices.h"
#include "theme.h"

#include <QApplication>
#include <QAudioFormat>
#include <QAudioOutput>
#include <QDate>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMediaPlayer>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <complex>
#include <functional>
#include <vector>

namespace {

QString durationText(qint64 ms)
{
    const qint64 total = qMax<qint64>(0, ms);
    const qint64 seconds = total / 1000;
    const qint64 tenths = (total % 1000) / 100;
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    const qint64 secs = seconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(secs, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2.%3")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(secs, 2, 10, QLatin1Char('0'))
        .arg(tenths);
}

QString dateText(const QDateTime &created)
{
    if (!created.isValid())
        return {};
    return created.toString(QStringLiteral("yyyy年M月d日 HH:mm"));
}

QString timeLabel(qint64 ms, int decimals)
{
    const qint64 total = qMax<qint64>(0, ms);
    const qint64 seconds = total / 1000;
    QString text = QStringLiteral("%1:%2")
                       .arg(seconds / 60, 2, 10, QLatin1Char('0'))
                       .arg(seconds % 60, 2, 10, QLatin1Char('0'));
    if (decimals > 0) {
        const qint64 fraction = decimals == 1 ? (total % 1000) / 100 : (total % 1000) / 10;
        text += QLatin1Char('.');
        text += QStringLiteral("%1").arg(fraction, decimals, 10, QLatin1Char('0'));
    }
    return text;
}

// 波形和声谱共用同一条时间轴：绘图区左右边界必须完全一致。
constexpr int kPlotLeft = 38;
constexpr int kPlotRight = 10;

QRectF wavePlotRect(const QRect &rect)
{
    return QRectF(rect).adjusted(kPlotLeft, 10, -kPlotRight, -26);
}

QRectF specPlotRect(const QRect &rect)
{
    return QRectF(rect).adjusted(kPlotLeft, 8, -kPlotRight, -8);
}

void fft(std::vector<std::complex<float>> &a)
{
    const int n = int(a.size());
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const float angle = -2.0f * float(M_PI) / float(len);
        const std::complex<float> step(std::cos(angle), std::sin(angle));
        for (int i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (int j = 0; j < len / 2; ++j) {
                const std::complex<float> u = a[i + j];
                const std::complex<float> v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= step;
            }
        }
    }
}

QRgb spectrogramColor(float value)
{
    struct Stop {
        float at;
        int r;
        int g;
        int b;
    };
    static const Stop stops[] = {
        {0.00f, 10, 6, 24},    {0.16f, 38, 14, 72},   {0.34f, 92, 24, 108},
        {0.50f, 142, 33, 100}, {0.66f, 198, 56, 68},  {0.80f, 240, 108, 30},
        {0.91f, 252, 176, 54}, {1.00f, 255, 242, 170},
    };
    value = qBound(0.0f, value, 1.0f);
    const int count = int(sizeof(stops) / sizeof(stops[0]));
    for (int i = 1; i < count; ++i) {
        if (value <= stops[i].at) {
            const float span = stops[i].at - stops[i - 1].at;
            const float t = span > 0.0f ? (value - stops[i - 1].at) / span : 0.0f;
            const auto mix = [t](int a, int b) { return int(a + (b - a) * t); };
            return qRgb(mix(stops[i - 1].r, stops[i].r), mix(stops[i - 1].g, stops[i].g),
                        mix(stops[i - 1].b, stops[i].b));
        }
    }
    return qRgb(255, 242, 170);
}

} // namespace

class DetailWaveform : public QWidget
{
public:
    explicit DetailWaveform(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMinimumHeight(160);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setCursor(Qt::IBeamCursor);
    }

    void setAudio(const QByteArray &pcm16, int sampleRate)
    {
        m_pcm = pcm16;
        m_rate = sampleRate;
        m_duration = sampleRate > 0 ? qint64(pcm16.size() / 2) * 1000 / sampleRate : 0;
        m_viewStart = 0;
        m_viewEnd = m_duration;
        m_playhead = 0;
        m_selectionStart = m_selectionEnd = -1;
        m_dragging = false;
        m_panning = false;
        update();
    }

    void setDuration(qint64 durationMs)
    {
        if (durationMs <= 0 || durationMs == m_duration)
            return;
        m_duration = durationMs;
        m_playhead = qBound<qint64>(0, m_playhead, m_duration);
        if (m_viewEnd <= m_viewStart)
            m_viewEnd = m_duration;
        m_viewEnd = qMin(m_viewEnd, m_duration);
        if (m_viewEnd <= m_viewStart) {
            m_viewStart = 0;
            m_viewEnd = m_duration;
        }
        update();
    }

    qint64 duration() const { return m_duration; }

    void setView(qint64 start, qint64 end)
    {
        if (m_duration <= 0)
            return;
        start = qBound<qint64>(0, start, m_duration);
        end = qBound<qint64>(start + 1, end, m_duration);
        if (start == m_viewStart && end == m_viewEnd)
            return;
        m_viewStart = start;
        m_viewEnd = end;
        update();
    }

    void setPlayhead(qint64 ms)
    {
        m_playhead = qBound<qint64>(0, ms, m_duration);
        update();
    }

    void setSelection(qint64 startMs, qint64 endMs)
    {
        m_selectionStart = qMax<qint64>(-1, startMs);
        m_selectionEnd = qMax<qint64>(-1, endMs);
        update();
    }

    void clearSelection()
    {
        m_selectionStart = m_selectionEnd = -1;
        update();
    }

    std::function<void(qint64, qint64)> onSelection;
    std::function<void(qint64)> onSeek;
    std::function<void(qint64, qint64)> onViewChange;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);
        const QPalette pal = palette();
        const QColor accent = pal.color(QPalette::Highlight);
        const QColor quiet = pal.color(QPalette::Mid);
        const QColor placeholder = pal.color(QPalette::PlaceholderText);

        painter.fillRect(rect(), pal.color(QPalette::Base));
        painter.setPen(QPen(quiet, 1));
        painter.drawRect(rect().adjusted(0, 0, -1, -1));

        const QRectF wave = wavePlotRect(rect());
        if (wave.width() < 10 || wave.height() < 10)
            return;

        if (m_pcm.size() < 2 || m_duration <= 0 || m_rate <= 0) {
            painter.setPen(placeholder);
            painter.drawText(wave, Qt::AlignCenter, QStringLiteral("无法显示波形"));
            return;
        }

        const qint64 frames = m_pcm.size() / 2;
        const qint64 span = qMax<qint64>(1, m_viewEnd - m_viewStart);
        const qint64 from = qint64(m_viewStart) * m_rate / 1000;
        const qint64 to = qMax(from + 1, qint64(m_viewEnd) * m_rate / 1000);
        const int columns = int(wave.width());
        const auto *samples = reinterpret_cast<const qint16 *>(m_pcm.constData());

        const qreal middle = wave.center().y();
        painter.setPen(QPen(quiet, 1));
        painter.drawLine(QPointF(wave.left(), middle), QPointF(wave.right(), middle));

        painter.setPen(QPen(accent, 1));
        for (int x = 0; x < columns; ++x) {
            qint64 begin = from + qint64(x) * (to - from) / columns;
            qint64 end = from + qint64(x + 1) * (to - from) / columns;
            end = qMax(end, begin + 1);
            begin = qBound<qint64>(0, begin, frames);
            end = qBound<qint64>(0, end, frames);
            if (end <= begin)
                continue;
            float low = 0.0f;
            float high = 0.0f;
            for (qint64 i = begin; i < end; ++i) {
                const float value = qFromLittleEndian<qint16>(
                                        reinterpret_cast<const char *>(samples + i))
                    / 32768.0f;
                low = qMin(low, value);
                high = qMax(high, value);
            }
            const qreal y1 = middle - high * (wave.height() * 0.48);
            const qreal y2 = middle - low * (wave.height() * 0.48);
            painter.drawLine(QPointF(wave.left() + x, y1), QPointF(wave.left() + x, y2));
        }

        const auto xFor = [&](qint64 ms) {
            return wave.left() + qreal(ms - m_viewStart) / qreal(span) * wave.width();
        };

        if (m_selectionStart >= 0 && m_selectionEnd > m_selectionStart
            && m_selectionEnd > m_viewStart && m_selectionStart < m_viewEnd) {
            const qreal x1 = xFor(qMax(m_selectionStart, m_viewStart));
            const qreal x2 = xFor(qMin(m_selectionEnd, m_viewEnd));
            QColor fill = accent;
            fill.setAlpha(45);
            painter.fillRect(QRectF(x1, wave.top(), x2 - x1, wave.height()), fill);
            painter.setPen(QPen(accent, 1));
            painter.drawLine(QPointF(x1, wave.top()), QPointF(x1, wave.bottom()));
            painter.drawLine(QPointF(x2, wave.top()), QPointF(x2, wave.bottom()));
        }

        if (m_playhead >= m_viewStart && m_playhead <= m_viewEnd) {
            const qreal x = xFor(m_playhead);
            painter.setPen(QPen(accent, 2));
            painter.drawLine(QPointF(x, wave.top()), QPointF(x, wave.bottom()));
        }

        // 时间刻度跟着可视区间走，缩放后自动换成更细的步长。
        static const qint64 candidates[] = {50,   100,   200,   500,   1000,  2000,   5000,
                                            10000, 15000, 30000, 60000, 120000, 300000, 600000};
        qint64 step = candidates[sizeof(candidates) / sizeof(candidates[0]) - 1];
        for (const qint64 candidate : candidates) {
            if (span / candidate <= 7) {
                step = candidate;
                break;
            }
        }
        const int decimals = step >= 1000 ? 0 : (step >= 100 ? 1 : 2);
        const qint64 firstTick = ((m_viewStart + step - 1) / step) * step;
        painter.setPen(QPen(quiet, 1));
        painter.setFont(font());
        for (qint64 t = firstTick; t <= m_viewEnd; t += step) {
            const qreal x = xFor(t);
            painter.setPen(QPen(quiet, 1));
            painter.drawLine(QPointF(x, wave.bottom()), QPointF(x, wave.bottom() + 4));
            QRectF label(x - 32, wave.bottom() + 4, 64, 18);
            if (label.left() < 4)
                label.moveLeft(4);
            if (label.right() > width() - 4)
                label.moveRight(width() - 4);
            painter.setPen(placeholder);
            painter.drawText(label, Qt::AlignHCenter | Qt::AlignTop, timeLabel(t, decimals));
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (m_duration <= 0)
            return;
        if (event->button() == Qt::MiddleButton) {
            m_panning = true;
            m_panX = event->position().x();
            m_panStart = m_viewStart;
            m_panEnd = m_viewEnd;
            setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
        if (event->button() != Qt::LeftButton)
            return;
        m_dragStartX = int(event->position().x());
        m_dragStartMs = qint64(msAt(event->position().x()));
        m_dragging = true;
        m_dragMoved = false;
        m_selectionStart = m_selectionEnd = m_dragStartMs;
        update();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_panning) {
            const qreal width = wavePlotRect(rect()).width();
            if (width > 0) {
                const qint64 span = m_panEnd - m_panStart;
                const qint64 delta = qint64(-(event->position().x() - m_panX) / width * span);
                requestView(m_panStart + delta, m_panEnd + delta);
            }
            return;
        }
        if (!m_dragging)
            return;
        if (qAbs(int(event->position().x()) - m_dragStartX) > 2)
            m_dragMoved = true;
        const qint64 current = qint64(msAt(event->position().x()));
        m_selectionStart = qMin(m_dragStartMs, current);
        m_selectionEnd = qMax(m_dragStartMs, current);
        if (m_dragMoved && onSelection)
            onSelection(m_selectionStart, m_selectionEnd);
        update();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::MiddleButton && m_panning) {
            m_panning = false;
            setCursor(Qt::IBeamCursor);
            event->accept();
            return;
        }
        if (event->button() != Qt::LeftButton || !m_dragging)
            return;
        m_dragging = false;
        if (!m_dragMoved) {
            m_selectionStart = m_selectionEnd = -1;
            if (onSelection)
                onSelection(-1, -1);
            if (onSeek)
                onSeek(m_dragStartMs);
        } else if (onSelection) {
            onSelection(m_selectionStart, m_selectionEnd);
        }
        update();
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_duration > 0) {
            requestView(0, m_duration);
            event->accept();
        }
    }

    void wheelEvent(QWheelEvent *event) override
    {
        if (m_duration <= 0 || event->angleDelta().y() == 0)
            return;
        zoomOrPan(event, m_viewStart, m_viewEnd);
        event->accept();
    }

private:
    void zoomOrPan(QWheelEvent *event, qint64 start, qint64 end)
    {
        const qreal steps = event->angleDelta().y() / 120.0;
        const qint64 span = end - start;
        if (event->modifiers() & Qt::ShiftModifier) {
            const qint64 delta = qint64(-steps * span * 0.12);
            requestView(start + delta, end + delta);
            return;
        }

        const qint64 minSpan = qMin<qint64>(m_duration, qMax<qint64>(200, m_duration / 32));
        const qint64 newSpan = qBound<qint64>(
            minSpan, qint64(std::llround(span * std::pow(1.25, -steps))), m_duration);
        if (newSpan == span)
            return;
        const qreal anchor = msAt(event->position().x());
        const qreal ratio = (anchor - start) / qreal(span);
        qint64 newStart = qint64(std::llround(anchor - ratio * newSpan));
        newStart = qBound<qint64>(0, newStart, m_duration - newSpan);
        requestView(newStart, newStart + newSpan);
    }

    qreal msAt(qreal x) const
    {
        const QRectF wave = wavePlotRect(rect());
        if (wave.width() <= 0 || m_duration <= 0)
            return 0.0;
        const qreal ratio = qBound(0.0, (x - wave.left()) / wave.width(), 1.0);
        return m_viewStart + ratio * (m_viewEnd - m_viewStart);
    }

    void requestView(qint64 start, qint64 end)
    {
        if (m_duration <= 0 || end <= start)
            return;
        const qint64 span = end - start;
        start = qBound<qint64>(0, start, qMax<qint64>(0, m_duration - span));
        if (onViewChange)
            onViewChange(start, start + span);
        else
            setView(start, start + span);
    }

    QByteArray m_pcm;
    int m_rate = 0;
    qint64 m_duration = 0;
    qint64 m_viewStart = 0;
    qint64 m_viewEnd = 0;
    qint64 m_playhead = 0;
    qint64 m_selectionStart = -1;
    qint64 m_selectionEnd = -1;
    int m_dragStartX = 0;
    qint64 m_dragStartMs = 0;
    bool m_dragging = false;
    bool m_dragMoved = false;
    bool m_panning = false;
    qreal m_panX = 0.0;
    qint64 m_panStart = 0;
    qint64 m_panEnd = 0;
};

class StaticSpectrogram : public QWidget
{
public:
    explicit StaticSpectrogram(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMinimumHeight(180);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setCursor(Qt::OpenHandCursor);
    }

    void setAudio(const QByteArray &pcm16, int sampleRate)
    {
        m_image = QImage();
        m_duration = sampleRate > 0 ? qint64(pcm16.size() / 2) * 1000 / sampleRate : 0;
        m_viewStart = 0;
        m_viewEnd = m_duration;
        if (pcm16.size() < 2048 || sampleRate <= 0) {
            update();
            return;
        }

        const int frames = pcm16.size() / 2;
        // 分辨率决定放大后的清晰度：短录音约 2 列/毫秒，长录音封顶 6000 列。
        const int columns = qBound(1200, int(m_duration / 2), 6000);
        const int rows = 180;
        const int fftSize = 1024;
        m_image = QImage(columns, rows, QImage::Format_RGB32);
        m_image.fill(spectrogramColor(0.0f));

        const auto *samples = reinterpret_cast<const qint16 *>(pcm16.constData());
        for (int col = 0; col < columns; ++col) {
            const int center = qint64(col) * frames / columns;
            const int start = center - fftSize / 2;
            std::vector<std::complex<float>> buffer(fftSize, 0.0f);
            for (int i = 0; i < fftSize; ++i) {
                const int index = start + i;
                if (index < 0 || index >= frames)
                    continue;
                const float value = qFromLittleEndian<qint16>(
                                        reinterpret_cast<const char *>(samples + index))
                    / 32768.0f;
                const float window =
                    0.5f * (1.0f - std::cos(2.0f * float(M_PI) * i / (fftSize - 1)));
                buffer[i] = value * window;
            }
            fft(buffer);

            QVector<float> magnitudes(rows, 0.0f);
            const float maxFreq = qMin(8000.0f, sampleRate / 2.0f);
            const float logSpan = std::log(maxFreq / 60.0f);
            for (int k = 1; k < fftSize / 2; ++k) {
                const float freq = float(k) * sampleRate / fftSize;
                if (freq < 60.0f || freq > maxFreq)
                    continue;
                const float magnitude = std::abs(buffer[k]) * 2.0f / fftSize;
                const float db = 20.0f * std::log10(magnitude + 1e-8f);
                const float normalized = qBound(0.0f, (db + 78.0f) / 78.0f, 1.0f);
                const float position = std::log(maxFreq / freq) / logSpan;
                const int row = qBound(0, int(position * (rows - 1)), rows - 1);
                magnitudes[row] = qMax(magnitudes[row], normalized);
            }
            for (int row = 0; row < rows; ++row) {
                auto *line = reinterpret_cast<QRgb *>(m_image.scanLine(row));
                line[col] = spectrogramColor(std::pow(magnitudes[row], 0.82f));
            }
        }
        update();
    }

    void setView(qint64 start, qint64 end)
    {
        if (m_duration <= 0)
            return;
        start = qBound<qint64>(0, start, m_duration);
        end = qBound<qint64>(start + 1, end, m_duration);
        if (start == m_viewStart && end == m_viewEnd)
            return;
        m_viewStart = start;
        m_viewEnd = end;
        update();
    }

    void setPlayhead(qint64 ms)
    {
        m_playhead = qBound<qint64>(0, ms, m_duration);
        update();
    }

    void setSelection(qint64 startMs, qint64 endMs)
    {
        m_selectionStart = qMax<qint64>(-1, startMs);
        m_selectionEnd = qMax<qint64>(-1, endMs);
        update();
    }

    void clearSelection()
    {
        m_selectionStart = m_selectionEnd = -1;
        update();
    }

    std::function<void(qint64, qint64)> onViewChange;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const QPalette pal = palette();
        painter.fillRect(rect(), pal.color(QPalette::Base));
        painter.setPen(QPen(pal.color(QPalette::Mid), 1));
        painter.drawRect(rect().adjusted(0, 0, -1, -1));

        const QRectF plot = specPlotRect(rect());
        if (m_image.isNull() || m_duration <= 0) {
            painter.setPen(pal.color(QPalette::PlaceholderText));
            painter.drawText(plot, Qt::AlignCenter, QStringLiteral("无法显示声谱"));
            return;
        }

        const qreal duration = m_duration;
        const qreal sourceLeft =
            qBound(0.0, m_viewStart / duration * m_image.width(), qreal(m_image.width() - 1));
        const qreal sourceWidth = qBound(
            1.0, (m_viewEnd - m_viewStart) / duration * m_image.width(),
            m_image.width() - sourceLeft);
        painter.drawImage(plot, m_image, QRectF(sourceLeft, 0, sourceWidth, m_image.height()));

        const qreal span = qMax<qint64>(1, m_viewEnd - m_viewStart);
        const auto xFor = [&](qint64 ms) {
            return plot.left() + qreal(ms - m_viewStart) / span * plot.width();
        };

        const QColor accent = pal.color(QPalette::Highlight);
        if (m_selectionStart >= 0 && m_selectionEnd > m_selectionStart
            && m_selectionEnd > m_viewStart && m_selectionStart < m_viewEnd) {
            const qreal x1 = xFor(qMax(m_selectionStart, m_viewStart));
            const qreal x2 = xFor(qMin(m_selectionEnd, m_viewEnd));
            QColor fill = accent;
            fill.setAlpha(38);
            painter.fillRect(QRectF(x1, plot.top(), x2 - x1, plot.height()), fill);
            painter.setPen(QPen(accent, 1));
            painter.drawLine(QPointF(x1, plot.top()), QPointF(x1, plot.bottom()));
            painter.drawLine(QPointF(x2, plot.top()), QPointF(x2, plot.bottom()));
        }

        if (m_playhead >= m_viewStart && m_playhead <= m_viewEnd) {
            const qreal x = xFor(m_playhead);
            painter.setPen(QPen(accent, 2));
            painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        }

        const float maxFreq = 8000.0f;
        const float logSpan = std::log(maxFreq / 60.0f);
        painter.setFont(font());
        painter.setPen(pal.color(QPalette::PlaceholderText));
        const QVector<int> marks = plot.height() < 150.0
            ? QVector<int>{125, 250, 500, 1000, 2000, 4000}
            : QVector<int>{125, 250, 500, 1000, 2000, 4000, 8000};
        for (const int mark : marks) {
            const qreal position = std::log(maxFreq / float(mark)) / logSpan;
            const qreal y = qBound(plot.top() + 8, plot.top() + position * plot.height(),
                                   plot.bottom() - 8);
            const QString label = mark >= 1000 ? QStringLiteral("%1k").arg(mark / 1000)
                                               : QString::number(mark);
            painter.drawText(QRectF(2, y - 8, kPlotLeft - 8, 16),
                             Qt::AlignRight | Qt::AlignVCenter, label);
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (m_duration <= 0)
            return;
        if (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) {
            m_panning = true;
            m_panX = event->position().x();
            m_panStart = m_viewStart;
            m_panEnd = m_viewEnd;
            setCursor(Qt::ClosedHandCursor);
            event->accept();
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_panning)
            return;
        const qreal width = specPlotRect(rect()).width();
        if (width <= 0)
            return;
        const qint64 span = m_panEnd - m_panStart;
        const qint64 delta = qint64(-(event->position().x() - m_panX) / width * span);
        requestView(m_panStart + delta, m_panEnd + delta);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (!m_panning)
            return;
        if (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) {
            m_panning = false;
            setCursor(Qt::OpenHandCursor);
            event->accept();
        }
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_duration > 0) {
            requestView(0, m_duration);
            event->accept();
        }
    }

    void wheelEvent(QWheelEvent *event) override
    {
        if (m_duration <= 0 || event->angleDelta().y() == 0)
            return;
        const qreal steps = event->angleDelta().y() / 120.0;
        const qint64 span = m_viewEnd - m_viewStart;
        if (event->modifiers() & Qt::ShiftModifier) {
            const qint64 delta = qint64(-steps * span * 0.12);
            requestView(m_viewStart + delta, m_viewEnd + delta);
            event->accept();
            return;
        }

        const qint64 minSpan = qMin<qint64>(m_duration, qMax<qint64>(200, m_duration / 32));
        const qint64 newSpan = qBound<qint64>(
            minSpan, qint64(std::llround(span * std::pow(1.25, -steps))), m_duration);
        if (newSpan == span)
            return;
        const qreal anchor = msAt(event->position().x());
        const qreal ratio = (anchor - m_viewStart) / qreal(span);
        qint64 newStart = qint64(std::llround(anchor - ratio * newSpan));
        newStart = qBound<qint64>(0, newStart, m_duration - newSpan);
        requestView(newStart, newStart + newSpan);
        event->accept();
    }

private:
    qreal msAt(qreal x) const
    {
        const QRectF plot = specPlotRect(rect());
        if (plot.width() <= 0 || m_duration <= 0)
            return 0.0;
        const qreal ratio = qBound(0.0, (x - plot.left()) / plot.width(), 1.0);
        return m_viewStart + ratio * (m_viewEnd - m_viewStart);
    }

    void requestView(qint64 start, qint64 end)
    {
        if (m_duration <= 0 || end <= start)
            return;
        const qint64 span = end - start;
        start = qBound<qint64>(0, start, qMax<qint64>(0, m_duration - span));
        if (onViewChange)
            onViewChange(start, start + span);
        else
            setView(start, start + span);
    }

    QImage m_image;
    qint64 m_duration = 0;
    qint64 m_viewStart = 0;
    qint64 m_viewEnd = 0;
    qint64 m_playhead = 0;
    qint64 m_selectionStart = -1;
    qint64 m_selectionEnd = -1;
    bool m_panning = false;
    qreal m_panX = 0.0;
    qint64 m_panStart = 0;
    qint64 m_panEnd = 0;
};

RecordingDetailDialog::RecordingDetailDialog(const Recording &recording,
                                             const Settings &settings, QWidget *parent)
    : QDialog(parent)
    , m_recording(recording)
    , m_settings(settings)
{
    setWindowTitle(QStringLiteral("录音详情"));
    setModal(true);
    resize(900, 680);

    auto *back = new QPushButton(theme::icon(QStringLiteral("back")),
                                 QStringLiteral("返回"));
    QFont titleFont = font();
    titleFont.setPointSizeF(titleFont.pointSizeF() + 2.0);
    titleFont.setBold(true);
    m_title = new QLabel(recording.title);
    m_title->setFont(titleFont);
    m_meta = new QLabel(QStringLiteral("%1 · %2 Hz · %3 位 · %4 声道")
                            .arg(recording.createdAt.isValid() ? dateText(recording.createdAt)
                                                               : QString())
                            .arg(recording.sampleRate)
                            .arg(recording.bits)
                            .arg(recording.channels));
    m_meta->setForegroundRole(QPalette::PlaceholderText);
    m_time = new QLabel;
    m_time->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    auto *titleBox = new QVBoxLayout;
    titleBox->setSpacing(2);
    titleBox->addWidget(m_title);
    titleBox->addWidget(m_meta);

    auto *header = new QHBoxLayout;
    header->setSpacing(12);
    header->addWidget(back, 0, Qt::AlignTop);
    header->addLayout(titleBox, 1);
    header->addWidget(m_time, 0, Qt::AlignTop);

    m_wave = new DetailWaveform;
    m_wave->setMinimumHeight(180);
    m_wave->setMaximumHeight(220);
    m_spectrogram = new StaticSpectrogram;
    m_spectrogram->setMinimumHeight(190);
    m_spectrogram->setMaximumHeight(260);

    m_selectionText = new QLabel(QStringLiteral("滚轮缩放 · 拖动声谱图平移 · 双击复位；在波形上拖动选择一段"));
    m_selectionText->setForegroundRole(QPalette::PlaceholderText);
    m_selectionText->setToolTip(QStringLiteral("缩放后可在声谱图上拖动平移；在波形上拖动选择片段"));

    m_zoomText = new QLabel(QStringLiteral("1.0×"));
    m_zoomText->setForegroundRole(QPalette::PlaceholderText);
    m_zoomText->setToolTip(QStringLiteral("鼠标滚轮缩放，拖动声谱图平移，双击复位"));

    m_play = new QPushButton(theme::icon(QStringLiteral("play")), QStringLiteral("播放"));
    m_playSelection = new QPushButton(QStringLiteral("播放选区"));
    m_clearSelection = new QPushButton(QStringLiteral("清除选区"));
    m_fit = new QPushButton(QStringLiteral("全览"));
    m_fit->setToolTip(QStringLiteral("显示整段录音"));
    m_playSelection->setEnabled(false);
    m_clearSelection->setEnabled(false);
    m_fit->setEnabled(false);
    for (QPushButton *button : {m_play, m_playSelection, m_clearSelection, m_fit})
        button->setFixedHeight(34);

    m_status = new QLabel;
    m_status->setForegroundRole(QPalette::PlaceholderText);
    m_status->setWordWrap(true);

    auto *hintRow = new QHBoxLayout;
    hintRow->setSpacing(12);
    hintRow->addWidget(m_selectionText, 1);
    hintRow->addWidget(m_zoomText);

    auto *controls = new QHBoxLayout;
    controls->addWidget(m_play);
    controls->addWidget(m_playSelection);
    controls->addWidget(m_clearSelection);
    controls->addWidget(m_fit);
    controls->addStretch(1);
    controls->addWidget(m_status, 2);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(12);
    layout->addLayout(header);
    layout->addWidget(m_wave);
    layout->addWidget(m_spectrogram);
    layout->addLayout(hintRow);
    layout->addLayout(controls);

    connect(back, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_play, &QPushButton::clicked, this, &RecordingDetailDialog::togglePlayback);
    connect(m_playSelection, &QPushButton::clicked, this,
            &RecordingDetailDialog::playSelection);
    connect(m_clearSelection, &QPushButton::clicked, this,
            &RecordingDetailDialog::clearSelection);
    connect(m_fit, &QPushButton::clicked, this, [this] { applyView(0, m_duration); });

    m_player = new QMediaPlayer(this);
    m_audio = new QAudioOutput(this);
    m_player->setAudioOutput(m_audio);
    m_audio->setVolume(m_settings.volume / 100.0);
    m_audio->setDevice(audio::findOutput(m_settings.outputDevice));

    m_wave->onSelection = [this](qint64 start, qint64 end) {
        m_selectionStart = start;
        m_selectionEnd = end;
        m_spectrogram->setSelection(start, end);
        const bool valid = start >= 0 && end > start + 50;
        m_playSelection->setEnabled(valid);
        m_clearSelection->setEnabled(valid);
        m_selectionText->setText(
            valid ? QStringLiteral("选区 %1 – %2（%3）")
                        .arg(durationText(start), durationText(end), durationText(end - start))
                  : QStringLiteral("滚轮缩放 · 拖动声谱图平移 · 双击复位；在波形上拖动选择一段"));
        if (valid)
            m_status->setText(QStringLiteral("已选择片段，可点「播放选区」。"));
    };
    m_wave->onViewChange = [this](qint64 start, qint64 end) { applyView(start, end); };
    m_spectrogram->onViewChange = [this](qint64 start, qint64 end) { applyView(start, end); };
    m_wave->onSeek = [this](qint64 ms) {
        m_player->setPosition(qBound<qint64>(0, ms, qMax<qint64>(0, m_duration)));
    };

    connect(m_player, &QMediaPlayer::positionChanged, this, [this](qint64 position) {
        // 放大后播放头走出视野时自动翻页，保持当前位置可见。
        if (m_player->playbackState() == QMediaPlayer::PlayingState && m_duration > 0
            && (position < m_viewStart || position > m_viewEnd)) {
            const qint64 span = m_viewEnd - m_viewStart;
            applyView(position - span / 10, position + span * 9 / 10);
        }
        m_wave->setPlayhead(position);
        m_spectrogram->setPlayhead(position);
        updateTime(position, m_duration);
        if (m_selectionPlayback && m_selectionEnd > m_selectionStart
            && position >= m_selectionEnd) {
            m_selectionPlayback = false;
            m_player->pause();
            m_player->setPosition(m_selectionEnd);
            setStatus(QStringLiteral("选区播放完成"));
        }
    });
    connect(m_player, &QMediaPlayer::durationChanged, this, [this](qint64 duration) {
        if (duration > 0) {
            m_duration = duration;
            m_wave->setDuration(duration);
            if (m_viewEnd > m_duration || m_viewEnd <= m_viewStart)
                applyView(0, m_duration);
            updateViewUi();
            updateTime(m_player->position(), duration);
        }
    });
    connect(m_player, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState) {
        updatePlayButton();
    });
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia) {
            m_selectionPlayback = false;
            m_wave->setPlayhead(m_duration);
            m_spectrogram->setPlayhead(m_duration);
            updatePlayButton();
            setStatus(QStringLiteral("播放完成"));
        }
    });
    connect(m_player, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error, const QString &message) {
                setStatus(QStringLiteral("播放失败：%1").arg(message));
            });

    m_player->setSource(QUrl::fromLocalFile(recording.path));
    m_duration = recording.durationMs;
    m_viewStart = 0;
    m_viewEnd = qMax<qint64>(1, m_duration);
    m_wave->setDuration(m_duration);
    updateTime(0, m_duration);
    setStatus(QStringLiteral("正在载入波形与声谱…"));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    loadAudio();
    QApplication::restoreOverrideCursor();
}

void RecordingDetailDialog::wheelForTest(int steps)
{
    if (steps == 0 || m_duration <= 0)
        return;
    const QPointF pos(m_spectrogram->width() * 0.4, m_spectrogram->height() * 0.45);
    QWheelEvent event(pos, m_spectrogram->mapToGlobal(pos.toPoint()), QPoint(),
                      QPoint(0, steps * 120), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QApplication::sendEvent(m_spectrogram, &event);
}

void RecordingDetailDialog::zoomForTest(double factor)
{
    if (factor <= 0.0 || m_duration <= 0)
        return;
    const qint64 span = qMax<qint64>(1, qint64(m_duration / factor));
    const qint64 center = m_duration / 2;
    applyView(center - span / 2, center + span / 2);
}

void RecordingDetailDialog::selectRangeForTest(qint64 startMs, qint64 endMs)
{
    m_selectionStart = startMs;
    m_selectionEnd = endMs;
    if (startMs < m_viewStart || endMs > m_viewEnd)
        applyView(qMax<qint64>(0, startMs - 500), qMin(m_duration, endMs + 500));
    m_wave->setSelection(startMs, endMs);
    m_spectrogram->setSelection(startMs, endMs);
    if (m_wave->onSelection)
        m_wave->onSelection(startMs, endMs);
}

RecordingDetailDialog::~RecordingDetailDialog()
{
    if (m_player)
        m_player->stop();
}

void RecordingDetailDialog::loadAudio()
{
    const audiofile::DecodedAudio decoded = audiofile::decodeToMono16(m_recording.path);
    if (!decoded.ok()) {
        setStatus(QStringLiteral("无法生成波形/声谱：%1").arg(decoded.error));
        return;
    }
    m_wave->setAudio(decoded.pcm16, 16000);
    m_spectrogram->setAudio(decoded.pcm16, 16000);
    m_duration = decoded.durationMs;
    m_wave->setDuration(m_duration);
    m_viewStart = 0;
    m_viewEnd = qMax<qint64>(1, m_duration);
    m_wave->setView(m_viewStart, m_viewEnd);
    m_spectrogram->setView(m_viewStart, m_viewEnd);
    updateViewUi();
    updateTime(0, m_duration);
    setStatus(QStringLiteral("就绪，可播放整段或选区。"));
}

void RecordingDetailDialog::applyView(qint64 start, qint64 end)
{
    if (m_duration <= 0)
        return;
    const qint64 minSpan = qMin<qint64>(m_duration, 200);
    const qint64 span = qBound<qint64>(minSpan, end - start, m_duration);
    if (span <= 0)
        return;
    start = qBound<qint64>(0, start, qMax<qint64>(0, m_duration - span));
    end = start + span;
    if (start == m_viewStart && end == m_viewEnd)
        return;
    m_viewStart = start;
    m_viewEnd = end;
    m_wave->setView(start, end);
    m_spectrogram->setView(start, end);
    updateViewUi();
}

void RecordingDetailDialog::updateViewUi()
{
    const qint64 span = m_viewEnd - m_viewStart;
    if (m_duration <= 0 || span <= 0) {
        m_fit->setEnabled(false);
        m_zoomText->setText(QStringLiteral("1.0×"));
        return;
    }
    const bool full = m_viewStart <= 0 && m_viewEnd >= m_duration;
    m_fit->setEnabled(!full);
    m_zoomText->setText(
        QStringLiteral("%1×").arg(double(m_duration) / double(span), 0, 'f', 1));
}

void RecordingDetailDialog::togglePlayback()
{
    if (m_player->playbackState() == QMediaPlayer::PlayingState) {
        m_player->pause();
        return;
    }
    m_selectionPlayback = false;
    if (m_player->duration() > 0
        && m_player->position() >= m_player->duration() - 50) {
        m_player->setPosition(0);
    }
    m_player->play();
}

void RecordingDetailDialog::playSelection()
{
    const bool valid = m_selectionStart >= 0 && m_selectionEnd > m_selectionStart + 50;
    if (!valid) {
        setStatus(QStringLiteral("先在波形上拖动选择一段。"));
        return;
    }
    m_selectionPlayback = true;
    m_player->setPosition(m_selectionStart);
    m_player->play();
    setStatus(QStringLiteral("正在播放选区…"));
}

void RecordingDetailDialog::clearSelection()
{
    m_selectionStart = m_selectionEnd = -1;
    m_selectionPlayback = false;
    m_wave->clearSelection();
    m_spectrogram->clearSelection();
    m_playSelection->setEnabled(false);
    m_clearSelection->setEnabled(false);
    m_selectionText->setText(
        QStringLiteral("滚轮缩放 · 拖动声谱图平移 · 双击复位；在波形上拖动选择一段"));
    setStatus(QStringLiteral("已清除选区。"));
}

void RecordingDetailDialog::updatePlayButton()
{
    const bool playing = m_player->playbackState() == QMediaPlayer::PlayingState;
    m_play->setIcon(theme::icon(playing ? QStringLiteral("pause") : QStringLiteral("play")));
    m_play->setText(playing ? QStringLiteral("暂停") : QStringLiteral("播放"));
}

void RecordingDetailDialog::updateTime(qint64 position, qint64 duration)
{
    m_time->setText(QStringLiteral("%1 / %2").arg(durationText(position),
                                                  durationText(duration)));
}

void RecordingDetailDialog::setStatus(const QString &text)
{
    m_status->setText(text);
}
