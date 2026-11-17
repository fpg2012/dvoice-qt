#include "audiomonitor.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPalette>
#include <QTimer>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <vector>

namespace {

constexpr int kFftSize = 2048;
constexpr int kBins = 64;
constexpr int kMaxSeconds = 4;

// 声谱图：480 列约 10 秒，160 行按对数频率分布。
constexpr int kSpecWidth = 480;
constexpr int kSpecRows = 160;
constexpr int kSpecHopMs = 20;
constexpr float kSpecMinFreq = 60.0f;
constexpr float kSpecMaxFreq = 8000.0f;

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

QString formatDb(float db)
{
    const int rounded = int(std::lround(db));
    return QStringLiteral("%1 dBFS").arg(rounded);
}

// magma 风格的色标：数据可视化用，和界面 palette 无关。
QRgb spectrogramColor(float value)
{
    struct Stop {
        float at;
        int r;
        int g;
        int b;
    };
    static const Stop stops[] = {
        {0.00f, 10, 6, 24},   {0.16f, 38, 14, 72},  {0.34f, 92, 24, 108},
        {0.50f, 142, 33, 100}, {0.66f, 198, 56, 68}, {0.80f, 240, 108, 30},
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

AudioMonitor::AudioMonitor(QWidget *parent)
    : QWidget(parent)
    , m_bins(kBins, 0.0f)
    , m_repaint(new QTimer(this))
{
    setMinimumHeight(150);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_repaint->setSingleShot(true);
    m_repaint->setInterval(33);
    connect(m_repaint, &QTimer::timeout, this, [this] {
        if (m_dirty) {
            m_dirty = false;
            update();
        }
    });
    resetSpectrogram();
}

void AudioMonitor::resetSpectrogram()
{
    m_waterfall = QImage(kSpecWidth, kSpecRows, QImage::Format_RGB32);
    m_waterfall.fill(spectrogramColor(0.0f));
    m_specWindow.clear();
    m_specPending.clear();
    m_specColumns = 0;
}

void AudioMonitor::setSampleRate(int sampleRate)
{
    if (sampleRate <= 0 || sampleRate == m_rate)
        return;
    m_rate = sampleRate;
    m_samples.clear();
    m_remainder.clear();
    m_bins.fill(0.0f);
    resetSpectrogram();
    update();
}

void AudioMonitor::setMode(Mode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    m_bins.fill(0.0f);
    resetSpectrogram();
    update();
}

void AudioMonitor::setCaption(const QString &caption)
{
    m_caption = caption;
    update();
}

void AudioMonitor::clear()
{
    m_samples.clear();
    m_remainder.clear();
    m_bins.fill(0.0f);
    resetSpectrogram();
    m_dirty = true;
    update();
}

void AudioMonitor::feed(const QByteArray &pcm)
{
    if (pcm.isEmpty())
        return;

    QByteArray data = m_remainder + pcm;
    const int usable = data.size() & ~1;
    m_remainder = data.mid(usable);
    data.truncate(usable);

    const int count = usable / 2;
    m_samples.reserve(m_samples.size() + count);
    for (int i = 0; i < count; ++i) {
        const qint16 sample = qFromLittleEndian<qint16>(data.constData() + i * 2);
        const float value = sample / 32768.0f;
        m_samples.append(value);
        if (m_mode == Mode::Spectrogram)
            m_specPending.append(value);
    }

    const int limit = m_rate * kMaxSeconds;
    if (m_samples.size() > limit)
        m_samples.remove(0, m_samples.size() - limit);

    if (m_mode == Mode::Spectrogram)
        processSpectrogram();

    scheduleRepaint();
}

void AudioMonitor::processSpectrogram()
{
    const int hop = qMax(64, m_rate * kSpecHopMs / 1000);
    while (m_specPending.size() >= hop) {
        for (int i = 0; i < hop; ++i)
            m_specWindow.append(m_specPending[i]);
        m_specPending.remove(0, hop);
        if (m_specWindow.size() > kFftSize)
            m_specWindow.remove(0, m_specWindow.size() - kFftSize);
        pushSpectrogramColumn();
    }
}

void AudioMonitor::pushSpectrogramColumn()
{
    if (m_waterfall.isNull())
        resetSpectrogram();

    const int taken = qMin(m_specWindow.size(), kFftSize);
    if (taken < 64)
        return;

    std::vector<std::complex<float>> buffer(kFftSize, 0.0f);
    const int offset = kFftSize - taken;
    for (int i = 0; i < taken; ++i) {
        const float window =
            0.5f * (1.0f - std::cos(2.0f * float(M_PI) * i / float(kFftSize - 1)));
        buffer[offset + i] = m_specWindow[m_specWindow.size() - taken + i] * window;
    }

    fft(buffer);

    const float maxFreq = qMin(kSpecMaxFreq, m_rate / 2.0f);
    QVector<float> rows(kSpecRows, 0.0f);
    const float logSpan = std::log(maxFreq / kSpecMinFreq);
    for (int k = 1; k < kFftSize / 2; ++k) {
        const float freq = float(k) * m_rate / kFftSize;
        if (freq < kSpecMinFreq || freq > maxFreq)
            continue;
        const float magnitude = std::abs(buffer[k]) * 2.0f / kFftSize;
        const float db = 20.0f * std::log10(magnitude + 1e-8f);
        const float normalized = qBound(0.0f, (db + 78.0f) / 78.0f, 1.0f);
        const float position = std::log(maxFreq / freq) / logSpan;
        const int row = qBound(0, int(position * (kSpecRows - 1)), kSpecRows - 1);
        rows[row] = qMax(rows[row], normalized);
    }

    // 左移一列，把最新一列画在右边，形成滚动瀑布。
    const int shift = (kSpecWidth - 1) * int(sizeof(QRgb));
    for (int y = 0; y < kSpecRows; ++y) {
        auto *line = reinterpret_cast<QRgb *>(m_waterfall.scanLine(y));
        std::memmove(line, line + 1, shift);
        line[kSpecWidth - 1] = spectrogramColor(std::pow(rows[y], 0.82f));
    }
    m_specColumns = qMin(kSpecWidth, m_specColumns + 1);
}

void AudioMonitor::scheduleRepaint()
{
    m_dirty = true;
    if (!m_repaint->isActive())
        m_repaint->start();
}

void AudioMonitor::computeSpectrum()
{
    const int available = m_samples.size();
    if (available < 256) {
        for (float &bin : m_bins)
            bin *= 0.8f;
        return;
    }

    std::vector<std::complex<float>> buffer(kFftSize, 0.0f);
    const int taken = qMin(kFftSize, available);
    const int offset = available - taken;
    for (int i = 0; i < taken; ++i) {
        const float window = 0.5f * (1.0f - std::cos(2.0f * float(M_PI) * i / float(kFftSize - 1)));
        buffer[i] = m_samples[offset + i] * window;
    }

    fft(buffer);

    const float maxFreq = qMin(8000.0f, m_rate / 2.0f);
    const float minFreq = 60.0f;
    QVector<float> target(kBins, 0.0f);
    for (int k = 1; k < kFftSize / 2; ++k) {
        const float freq = float(k) * m_rate / kFftSize;
        if (freq < minFreq || freq > maxFreq)
            continue;
        const float magnitude = std::abs(buffer[k]) * 2.0f / kFftSize;
        const float db = 20.0f * std::log10(magnitude + 1e-7f);
        const float normalized = qBound(0.0f, (db + 65.0f) / 65.0f, 1.0f);
        const float position = std::log(freq / minFreq) / std::log(maxFreq / minFreq);
        const int bin = qBound(0, int(position * (kBins - 1)), kBins - 1);
        target[bin] = qMax(target[bin], normalized);
    }

    for (int i = 0; i < kBins; ++i)
        m_bins[i] = qMax(target[i], m_bins[i] * 0.84f);
}

void AudioMonitor::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QPalette pal = palette();
    const QColor background = pal.color(QPalette::Base);
    const QColor ink = pal.color(QPalette::Text);
    const QColor accent = pal.color(QPalette::Highlight);
    const QColor quiet = pal.color(QPalette::Mid);
    const QColor placeholder = pal.color(QPalette::PlaceholderText);

    const QRectF outer = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen(quiet, 1));
    painter.setBrush(background);
    painter.drawRoundedRect(outer, 8, 8);

    const QRectF content = outer.adjusted(12, 8, -12, -8);
    if (content.width() < 40 || content.height() < 60)
        return;

    QFont small = font();
    small.setPointSizeF(qMax(7.0, font().pointSizeF() - 2.0));
    const QFontMetrics smallMetrics(small);

    const qreal headerHeight = smallMetrics.height() + 4;
    const QRectF header(content.left(), content.top(), content.width(), headerHeight);
    const qreal waveRatio = m_mode == Mode::Spectrogram ? 0.42 : 0.60;
    const QRectF wave(content.left(), header.bottom() + 2, content.width(),
                      (content.height() - headerHeight) * waveRatio);
    const QRectF spec(content.left(), wave.bottom() + 8, content.width(),
                      content.bottom() - wave.bottom() - 8);

    painter.setFont(small);
    painter.setPen(placeholder);
    painter.drawText(header, Qt::AlignLeft | Qt::AlignVCenter,
                     m_caption.isEmpty() ? QStringLiteral("音频监视") : m_caption);

    // 计算电平。
    float db = -120.0f;
    const int levelCount = qMin(2048, m_samples.size());
    if (levelCount > 0) {
        double sum = 0.0;
        for (int i = m_samples.size() - levelCount; i < m_samples.size(); ++i)
            sum += double(m_samples[i]) * m_samples[i];
        const double rms = std::sqrt(sum / levelCount);
        db = rms > 1e-9 ? float(20.0 * std::log10(rms)) : -120.0f;
    }
    painter.setPen(db > -60.0f ? ink : placeholder);
    painter.drawText(header, Qt::AlignRight | Qt::AlignVCenter, formatDb(db));

    painter.setPen(QPen(quiet, 1, Qt::SolidLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(wave.adjusted(0.5, 0.5, -0.5, -0.5));
    painter.drawRect(spec.adjusted(0.5, 0.5, -0.5, -0.5));

    // 波形：每列取最小/最大值。
    painter.save();
    painter.setClipRect(wave);
    const qreal middle = wave.center().y();
    painter.setPen(QPen(quiet, 1));
    painter.drawLine(QPointF(wave.left(), middle), QPointF(wave.right(), middle));

    const int columns = int(wave.width());
    const int window = qMin(m_samples.size(), m_rate * 2);
    if (window >= 2 && columns > 0) {
        const int start = m_samples.size() - window;
        painter.setPen(QPen(accent, 1.2));
        for (int x = 0; x < columns; ++x) {
            const int from = start + qint64(x) * window / columns;
            const int to = qMax(from + 1, start + qint64(x + 1) * window / columns);
            float low = 0.0f;
            float high = 0.0f;
            for (int i = from; i < to && i < m_samples.size(); ++i) {
                low = qMin(low, m_samples[i]);
                high = qMax(high, m_samples[i]);
            }
            const qreal y1 = middle - high * (wave.height() * 0.46);
            const qreal y2 = middle - low * (wave.height() * 0.46);
            painter.drawLine(QPointF(wave.left() + x, y1), QPointF(wave.left() + x, y2));
        }
    } else {
        painter.setPen(placeholder);
        painter.drawText(wave, Qt::AlignCenter, QStringLiteral("等待音频…"));
    }
    painter.restore();

    const float maxFreq = qMin(8000.0f, m_rate / 2.0f);
    const QVector<int> barMarks = {0, 1000, 2000, 4000, 8000};
    const QVector<int> spectralMarks = spec.height() < 150.0
        ? QVector<int>{125, 250, 500, 1000, 2000, 4000}
        : QVector<int>{125, 250, 500, 1000, 2000, 4000, 8000};
    const float logSpan = std::log(maxFreq / 60.0f);

    if (m_mode == Mode::Bars) {
        computeSpectrum();
        painter.save();
        painter.setClipRect(spec);
        const qreal barWidth = spec.width() / kBins;
        QLinearGradient gradient(spec.topLeft(), spec.bottomLeft());
        QColor top = accent;
        top.setAlpha(210);
        QColor bottom = accent;
        bottom.setAlpha(50);
        gradient.setColorAt(0.0, top);
        gradient.setColorAt(1.0, bottom);
        painter.setPen(Qt::NoPen);
        painter.setBrush(gradient);
        for (int i = 0; i < kBins; ++i) {
            const qreal value = qBound(0.0f, m_bins[i], 1.0f);
            const qreal height = value * (spec.height() - 14);
            if (height < 0.6)
                continue;
            const QRectF bar(spec.left() + i * barWidth + 0.5, spec.bottom() - 12 - height,
                             qMax(1.0, barWidth - 1.2), height);
            painter.drawRect(bar);
        }
        painter.restore();

        painter.setFont(small);
        painter.setPen(placeholder);
        for (const int mark : barMarks) {
            if (mark > maxFreq)
                continue;
            const float position = mark == 0
                ? 0.0f
                : std::log(float(mark) / 60.0f) / logSpan;
            const qreal x = spec.left() + position * spec.width();
            const QString label = mark >= 1000 ? QStringLiteral("%1k").arg(mark / 1000)
                                               : QString::number(mark);
            painter.drawText(QRectF(x - 20, spec.bottom() - 12, 40, 12),
                             Qt::AlignCenter, label);
        }
    } else {
        painter.save();
        const QRectF waterfall = spec.adjusted(30, 1, -1, -1);
        painter.setClipRect(waterfall);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(waterfall, m_waterfall);
        painter.restore();

        painter.setFont(small);
        painter.setPen(placeholder);
        for (const int mark : spectralMarks) {
            if (mark > maxFreq)
                continue;
            const qreal position = std::log(maxFreq / float(mark)) / logSpan;
            const qreal y = qBound(spec.top() + 7, spec.top() + position * spec.height(),
                                   spec.bottom() - 7);
            const QString label = mark >= 1000 ? QStringLiteral("%1k").arg(mark / 1000)
                                               : QString::number(mark);
            painter.drawText(QRectF(spec.left() + 1, y - 7, 28, 14),
                             Qt::AlignRight | Qt::AlignVCenter, label);
        }
        if (m_specColumns > 0) {
            painter.setPen(QPen(accent, 1));
            painter.drawLine(QPointF(waterfall.right() - 1, waterfall.top()),
                             QPointF(waterfall.right() - 1, waterfall.bottom()));
        }
    }
}
