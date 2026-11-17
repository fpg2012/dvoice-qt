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

QString timeLabel(qint64 ms, bool withTenths)
{
    const qint64 total = qMax<qint64>(0, ms);
    const qint64 seconds = total / 1000;
    if (withTenths) {
        return QStringLiteral("%1:%2.%3")
            .arg(seconds / 60, 2, 10, QLatin1Char('0'))
            .arg(seconds % 60, 2, 10, QLatin1Char('0'))
            .arg((total % 1000) / 100);
    }
    return QStringLiteral("%1:%2")
        .arg(seconds / 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
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
        m_min.clear();
        m_max.clear();
        m_duration = 0;
        m_playhead = 0;
        m_selectionStart = m_selectionEnd = -1;
        if (pcm16.size() < 2 || sampleRate <= 0) {
            update();
            return;
        }

        const int frames = pcm16.size() / 2;
        m_duration = qint64(frames) * 1000 / sampleRate;
        const int buckets = 2400;
        m_min.resize(buckets);
        m_max.resize(buckets);
        const auto *samples = reinterpret_cast<const qint16 *>(pcm16.constData());
        for (int i = 0; i < buckets; ++i) {
            const int from = qint64(i) * frames / buckets;
            const int to = qMax(from + 1, int(qint64(i + 1) * frames / buckets));
            float low = 0.0f;
            float high = 0.0f;
            for (int j = from; j < to && j < frames; ++j) {
                const float value = qFromLittleEndian<qint16>(
                                        reinterpret_cast<const char *>(samples + j))
                    / 32768.0f;
                low = qMin(low, value);
                high = qMax(high, value);
            }
            m_min[i] = low;
            m_max[i] = high;
        }
        update();
    }

    void setDuration(qint64 durationMs)
    {
        if (durationMs > 0 && durationMs != m_duration) {
            m_duration = durationMs;
            update();
        }
    }

    qint64 duration() const { return m_duration; }

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

        const QRectF wave = QRectF(rect()).adjusted(10, 10, -10, -26);
        if (wave.width() < 10 || wave.height() < 10)
            return;

        if (m_max.isEmpty() || m_duration <= 0) {
            painter.setPen(placeholder);
            painter.drawText(wave, Qt::AlignCenter, QStringLiteral("无法显示波形"));
            return;
        }

        const qreal middle = wave.center().y();
        painter.setPen(QPen(quiet, 1));
        painter.drawLine(QPointF(wave.left(), middle), QPointF(wave.right(), middle));

        const int columns = int(wave.width());
        painter.setPen(QPen(accent, 1));
        for (int x = 0; x < columns; ++x) {
            const int bucket = qBound(0, x * m_max.size() / columns, m_max.size() - 1);
            const qreal y1 = middle - m_max[bucket] * wave.height() * 0.48;
            const qreal y2 = middle - m_min[bucket] * wave.height() * 0.48;
            painter.drawLine(QPointF(wave.left() + x, y1), QPointF(wave.left() + x, y2));
        }

        if (m_selectionStart >= 0 && m_selectionEnd > m_selectionStart) {
            const qreal x1 = wave.left() + qreal(m_selectionStart) / m_duration * wave.width();
            const qreal x2 = wave.left() + qreal(m_selectionEnd) / m_duration * wave.width();
            QColor fill = accent;
            fill.setAlpha(45);
            painter.fillRect(QRectF(x1, wave.top(), x2 - x1, wave.height()), fill);
            painter.setPen(QPen(accent, 1));
            painter.drawLine(QPointF(x1, wave.top()), QPointF(x1, wave.bottom()));
            painter.drawLine(QPointF(x2, wave.top()), QPointF(x2, wave.bottom()));
        }

        if (m_playhead >= 0) {
            const qreal x = wave.left() + qreal(m_playhead) / m_duration * wave.width();
            painter.setPen(QPen(accent, 2));
            painter.drawLine(QPointF(x, wave.top()), QPointF(x, wave.bottom()));
        }

        // 时间刻度。
        const qint64 candidates[] = {1000, 2000, 5000, 10000, 15000, 30000, 60000, 120000,
                                     300000, 600000};
        qint64 step = candidates[0];
        for (const qint64 candidate : candidates) {
            step = candidate;
            if (m_duration / candidate <= 8)
                break;
        }
        painter.setPen(QPen(quiet, 1));
        painter.setFont(font());
        const bool tenths = step < 1000;
        for (qint64 t = 0; t <= m_duration; t += step) {
            const qreal x = wave.left() + qreal(t) / m_duration * wave.width();
            painter.setPen(QPen(quiet, 1));
            painter.drawLine(QPointF(x, wave.bottom()), QPointF(x, wave.bottom() + 4));
            painter.setPen(placeholder);
            painter.drawText(QRectF(x - 24, wave.bottom() + 4, 48, 18),
                             Qt::AlignHCenter | Qt::AlignTop, timeLabel(t, tenths));
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || m_duration <= 0)
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

private:
    qreal msAt(qreal x) const
    {
        const QRectF wave = QRectF(rect()).adjusted(10, 10, -10, -26);
        if (wave.width() <= 0 || m_duration <= 0)
            return 0.0;
        return qBound(0.0, (x - wave.left()) / wave.width(), 1.0) * m_duration;
    }

    QVector<float> m_min;
    QVector<float> m_max;
    qint64 m_duration = 0;
    qint64 m_playhead = 0;
    qint64 m_selectionStart = -1;
    qint64 m_selectionEnd = -1;
    int m_dragStartX = 0;
    qint64 m_dragStartMs = 0;
    bool m_dragging = false;
    bool m_dragMoved = false;
};

class StaticSpectrogram : public QWidget
{
public:
    explicit StaticSpectrogram(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMinimumHeight(180);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setAudio(const QByteArray &pcm16, int sampleRate)
    {
        m_image = QImage();
        if (pcm16.size() < 2048 || sampleRate <= 0) {
            update();
            return;
        }

        const int frames = pcm16.size() / 2;
        const int columns = 1000;
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

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const QPalette pal = palette();
        painter.fillRect(rect(), pal.color(QPalette::Base));
        painter.setPen(QPen(pal.color(QPalette::Mid), 1));
        painter.drawRect(rect().adjusted(0, 0, -1, -1));

        const QRectF imageRect = QRectF(rect()).adjusted(34, 8, -8, -8);
        if (m_image.isNull()) {
            painter.setPen(pal.color(QPalette::PlaceholderText));
            painter.drawText(imageRect, Qt::AlignCenter, QStringLiteral("无法显示声谱"));
            return;
        }
        painter.drawImage(imageRect, m_image);

        const float maxFreq = 8000.0f;
        const float logSpan = std::log(maxFreq / 60.0f);
        painter.setFont(font());
        painter.setPen(pal.color(QPalette::PlaceholderText));
        for (const int mark : {1000, 2000, 4000, 8000}) {
            const qreal position = std::log(maxFreq / float(mark)) / logSpan;
            const qreal y = imageRect.top() + position * imageRect.height();
            painter.drawText(QRectF(2, y - 8, 28, 16), Qt::AlignRight | Qt::AlignVCenter,
                             QStringLiteral("%1k").arg(mark / 1000));
        }
    }

private:
    QImage m_image;
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

    m_selectionText = new QLabel(QStringLiteral("在波形上拖动选择一段，或单击定位"));
    m_selectionText->setForegroundRole(QPalette::PlaceholderText);

    m_play = new QPushButton(theme::icon(QStringLiteral("play")), QStringLiteral("播放"));
    m_playSelection = new QPushButton(QStringLiteral("播放选区"));
    m_clearSelection = new QPushButton(QStringLiteral("清除选区"));
    m_playSelection->setEnabled(false);
    m_clearSelection->setEnabled(false);
    for (QPushButton *button : {m_play, m_playSelection, m_clearSelection})
        button->setFixedHeight(34);

    m_status = new QLabel;
    m_status->setForegroundRole(QPalette::PlaceholderText);
    m_status->setWordWrap(true);

    auto *controls = new QHBoxLayout;
    controls->addWidget(m_play);
    controls->addWidget(m_playSelection);
    controls->addWidget(m_clearSelection);
    controls->addStretch(1);
    controls->addWidget(m_status, 2);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(12);
    layout->addLayout(header);
    layout->addWidget(m_wave);
    layout->addWidget(m_spectrogram);
    layout->addWidget(m_selectionText);
    layout->addLayout(controls);

    connect(back, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_play, &QPushButton::clicked, this, &RecordingDetailDialog::togglePlayback);
    connect(m_playSelection, &QPushButton::clicked, this,
            &RecordingDetailDialog::playSelection);
    connect(m_clearSelection, &QPushButton::clicked, this,
            &RecordingDetailDialog::clearSelection);

    m_player = new QMediaPlayer(this);
    m_audio = new QAudioOutput(this);
    m_player->setAudioOutput(m_audio);
    m_audio->setVolume(m_settings.volume / 100.0);
    m_audio->setDevice(audio::findOutput(m_settings.outputDevice));

    m_wave->onSelection = [this](qint64 start, qint64 end) {
        m_selectionStart = start;
        m_selectionEnd = end;
        const bool valid = start >= 0 && end > start + 50;
        m_playSelection->setEnabled(valid);
        m_clearSelection->setEnabled(valid);
        m_selectionText->setText(valid
                                     ? QStringLiteral("选区 %1 – %2（%3）")
                                           .arg(durationText(start), durationText(end),
                                                durationText(end - start))
                                     : QStringLiteral("在波形上拖动选择一段，或单击定位"));
        if (valid)
            m_status->setText(QStringLiteral("已选择片段，可点「播放选区」。"));
    };
    m_wave->onSeek = [this](qint64 ms) {
        m_player->setPosition(qBound<qint64>(0, ms, qMax<qint64>(0, m_duration)));
    };

    connect(m_player, &QMediaPlayer::positionChanged, this, [this](qint64 position) {
        m_wave->setPlayhead(position);
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
    m_wave->setDuration(m_duration);
    updateTime(0, m_duration);
    setStatus(QStringLiteral("正在载入波形与声谱…"));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    loadAudio();
    QApplication::restoreOverrideCursor();
}

void RecordingDetailDialog::selectRangeForTest(qint64 startMs, qint64 endMs)
{
    m_selectionStart = startMs;
    m_selectionEnd = endMs;
    m_wave->setSelection(startMs, endMs);
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
    updateTime(0, m_duration);
    setStatus(QStringLiteral("拖动波形选择片段，点「播放选区」；单击波形定位。"));
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
    m_playSelection->setEnabled(false);
    m_clearSelection->setEnabled(false);
    m_selectionText->setText(QStringLiteral("在波形上拖动选择一段，或单击定位"));
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
