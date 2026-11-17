#include "recordingcard.h"

#include "theme.h"

#include <QDate>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QToolButton>
#include <QStyle>
#include <QVBoxLayout>

#include <functional>

#include <cmath>

namespace {

QString durationText(qint64 ms)
{
    const qint64 seconds = qMax<qint64>(0, ms) / 1000;
    return QStringLiteral("%1:%2")
        .arg(seconds / 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QString dateText(const QDateTime &created)
{
    if (!created.isValid())
        return {};
    const QDate date = created.date();
    const QDate today = QDate::currentDate();
    const qint64 days = date.daysTo(today);
    if (days == 0)
        return QStringLiteral("今天 %1").arg(created.time().toString(QStringLiteral("HH:mm")));
    if (days == 1)
        return QStringLiteral("昨天 %1").arg(created.time().toString(QStringLiteral("HH:mm")));
    if (days > 1 && days < 7)
        return QStringLiteral("%1 天前").arg(days);
    if (date.year() == today.year())
        return QStringLiteral("%1月%2日").arg(date.month()).arg(date.day());
    return QStringLiteral("%1年%2月%3日").arg(date.year()).arg(date.month()).arg(date.day());
}

} // namespace

class WaveformView : public QWidget
{
public:
    explicit WaveformView(const QVector<float> &peaks, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_peaks(peaks)
    {
        setMinimumHeight(54);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setCursor(Qt::PointingHandCursor);
    }

    void setProgress(qreal value)
    {
        m_progress = qBound(0.0, value, 1.0);
        update();
    }

    std::function<void(qreal)> onSeek;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);
        const QPalette pal = palette();
        const QColor accent = pal.color(QPalette::Highlight);
        const QColor quiet = pal.color(QPalette::Mid);
        const QColor unplayed = pal.color(QPalette::PlaceholderText);
        const QColor placeholder = pal.color(QPalette::PlaceholderText);

        const QRectF area = QRectF(rect()).adjusted(1, 4, -1, -4);
        if (area.width() < 4 || area.height() < 4)
            return;
        const qreal middle = area.center().y();

        painter.setPen(QPen(quiet, 1));
        painter.drawLine(QPointF(area.left(), middle), QPointF(area.right(), middle));

        if (m_peaks.isEmpty()) {
            painter.setPen(placeholder);
            painter.drawText(area, Qt::AlignCenter, QStringLiteral("无波形"));
            return;
        }

        const qreal step = area.width() / m_peaks.size();
        const qreal playedX = area.left() + area.width() * m_progress;
        for (int i = 0; i < m_peaks.size(); ++i) {
            const qreal x = area.left() + (i + 0.5) * step;
            const qreal height = qMax(1.0, qBound(0.0f, m_peaks[i], 1.0f) * area.height() * 0.92);
            const bool played = x <= playedX;
            painter.setPen(QPen(played ? accent : unplayed, qMax(1.5, step * 0.62)));
            painter.drawLine(QPointF(x, middle - height / 2), QPointF(x, middle + height / 2));
        }

        if (m_progress > 0.001) {
            painter.setPen(QPen(accent, 1));
            painter.drawLine(QPointF(playedX, area.top()), QPointF(playedX, area.bottom()));
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && width() > 0 && onSeek) {
            const qreal ratio = qBound(0.0, event->position().x() / width(), 1.0);
            onSeek(ratio);
        }
        QWidget::mousePressEvent(event);
    }

private:
    QVector<float> m_peaks;
    qreal m_progress = 0.0;
};

RecordingCard::RecordingCard(const Recording &recording, QWidget *parent)
    : QFrame(parent)
    , m_recording(recording)
{
    setObjectName(QStringLiteral("recordingCard"));
    setFrameShape(QFrame::NoFrame);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    QFont titleFont = font();
    titleFont.setPointSizeF(titleFont.pointSizeF() + 1.2);
    titleFont.setBold(true);

    m_title = new QLabel(recording.title);
    m_title->setFont(titleFont);
    m_title->setTextInteractionFlags(Qt::NoTextInteraction);

    m_date = new QLabel(dateText(recording.createdAt));
    m_date->setForegroundRole(QPalette::PlaceholderText);

    m_duration = new QLabel(durationText(recording.durationMs));
    m_duration->setForegroundRole(QPalette::PlaceholderText);

    auto *meta = new QVBoxLayout;
    meta->setContentsMargins(0, 0, 0, 0);
    meta->setSpacing(2);
    meta->addWidget(m_title);
    meta->addWidget(m_date);

    auto *top = new QHBoxLayout;
    top->setContentsMargins(0, 0, 0, 0);
    top->addLayout(meta, 1);
    top->addWidget(m_duration, 0, Qt::AlignTop);

    m_wave = new WaveformView(recording.peaks);
    m_wave->setVisible(false);
    m_wave->onSeek = [this](qreal ratio) { emit seekRequested(ratio); };

    auto *deleteButton = new QToolButton;
    deleteButton->setIcon(theme::icon(QStringLiteral("clear")));
    deleteButton->setToolTip(QStringLiteral("删除录音"));
    deleteButton->setAutoRaise(true);

    auto *rewindButton = new QToolButton;
    rewindButton->setIcon(theme::icon(QStringLiteral("rewind")));
    rewindButton->setToolTip(QStringLiteral("后退 10 秒"));
    rewindButton->setAutoRaise(true);
    rewindButton->setIconSize(QSize(24, 24));

    m_play = new QToolButton;
    m_play->setObjectName(QStringLiteral("cardPlay"));
    m_play->setIcon(theme::icon(QStringLiteral("play")));
    m_play->setToolTip(QStringLiteral("播放"));
    m_play->setIconSize(QSize(24, 24));
    m_play->setFixedSize(46, 46);

    auto *forwardButton = new QToolButton;
    forwardButton->setIcon(theme::icon(QStringLiteral("forward")));
    forwardButton->setToolTip(QStringLiteral("前进 10 秒"));
    forwardButton->setAutoRaise(true);
    forwardButton->setIconSize(QSize(24, 24));

    auto *exportButton = new QToolButton;
    exportButton->setIcon(theme::icon(QStringLiteral("export")));
    exportButton->setToolTip(QStringLiteral("导出音频"));
    exportButton->setAutoRaise(true);

    auto *editButton = new QToolButton;
    editButton->setIcon(theme::icon(QStringLiteral("edit")));
    editButton->setToolTip(QStringLiteral("重命名"));
    editButton->setAutoRaise(true);

    m_controls = new QWidget;
    auto *bottom = new QHBoxLayout(m_controls);
    bottom->setContentsMargins(0, 0, 0, 0);
    bottom->setSpacing(8);
    bottom->addWidget(deleteButton);
    bottom->addStretch(1);
    bottom->addWidget(rewindButton);
    bottom->addWidget(m_play);
    bottom->addWidget(forwardButton);
    bottom->addStretch(1);
    bottom->addWidget(exportButton);
    bottom->addWidget(editButton);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 10, 18, 8);
    layout->setSpacing(6);
    layout->addLayout(top);
    layout->addWidget(m_wave);
    layout->addWidget(m_controls);

    m_controls->setVisible(false);
    for (QWidget *widget : {static_cast<QWidget *>(m_title), static_cast<QWidget *>(m_date),
                            static_cast<QWidget *>(m_duration), static_cast<QWidget *>(m_wave),
                            static_cast<QWidget *>(m_controls)})
        widget->installEventFilter(this);

    setStyleSheet(QStringLiteral(
        "QFrame#recordingCard { background: palette(alternate-base); border: 1px solid "
        "palette(mid); border-radius: 10px; }"
        "QFrame#recordingCard[selected=\"true\"] { border: 1px solid palette(highlight); }"
        "QToolButton#cardPlay { background: palette(button); border: 1px solid palette(mid); "
        "border-radius: 23px; }"
        "QToolButton#cardPlay:hover { background: palette(highlight); border-color: "
        "palette(highlight); }"
        "QLabel { background: transparent; }"));

    connect(m_play, &QToolButton::clicked, this, &RecordingCard::playRequested);
    connect(rewindButton, &QToolButton::clicked, this, [this] { emit skipRequested(-10000); });
    connect(forwardButton, &QToolButton::clicked, this, [this] { emit skipRequested(10000); });
    connect(exportButton, &QToolButton::clicked, this, &RecordingCard::exportRequested);
    connect(editButton, &QToolButton::clicked, this, &RecordingCard::renameRequested);
    connect(deleteButton, &QToolButton::clicked, this, &RecordingCard::deleteRequested);
}

void RecordingCard::setPlaying(bool playing)
{
    m_play->setIcon(theme::icon(playing ? QStringLiteral("pause") : QStringLiteral("play")));
    m_play->setToolTip(playing ? QStringLiteral("暂停") : QStringLiteral("播放"));
}

void RecordingCard::setProgress(qreal progress)
{
    m_wave->setProgress(progress);
}

void RecordingCard::setSelected(bool selected)
{
    if (m_selected == selected)
        return;
    m_selected = selected;
    m_wave->setVisible(selected);
    m_controls->setVisible(selected);
    setProperty("selected", selected);
    style()->unpolish(this);
    style()->polish(this);
    updateGeometry();
    update();
}

bool RecordingCard::eventFilter(QObject *watched, QEvent *event)
{
    Q_UNUSED(watched)
    if (event->type() == QEvent::MouseButtonPress) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton)
            emit clicked();
    } else if (event->type() == QEvent::MouseButtonDblClick) {
        emit doubleClicked();
        return true;
    }
    return QFrame::eventFilter(watched, event);
}

void RecordingCard::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
        emit clicked();
    QFrame::mousePressEvent(event);
}

void RecordingCard::mouseDoubleClickEvent(QMouseEvent *event)
{
    emit doubleClicked();
    QFrame::mouseDoubleClickEvent(event);
}
