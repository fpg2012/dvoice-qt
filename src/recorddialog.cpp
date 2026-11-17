#include "recorddialog.h"

#include "audiomonitor.h"
#include "core/audiodevices.h"
#include "core/recordingstore.h"
#include "theme.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QComboBox>
#include <QDesktopServices>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPalette>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>

namespace {

QString durationText(qint64 ms)
{
    const qint64 seconds = qMax<qint64>(0, ms) / 1000;
    return QStringLiteral("%1:%2")
        .arg(seconds / 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QString sizeText(qint64 bytes)
{
    if (bytes >= 1024 * 1024)
        return QStringLiteral("%1 MB").arg(bytes / 1024.0 / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 0);
}

QString dbText(qreal rms)
{
    if (rms <= 1e-6)
        return QStringLiteral("−∞ dB");
    return QStringLiteral("%1 dB").arg(int(std::lround(20.0 * std::log10(rms))));
}

} // namespace

RecordDialog::RecordDialog(RecordingStore *store, const Settings &settings, QWidget *parent)
    : QDialog(parent)
    , m_store(store)
    , m_settings(settings)
{
    setWindowTitle(QStringLiteral("录音"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(760, 560);

    m_device = new QComboBox;
    m_device->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_device->setMinimumContentsLength(24);
    m_device->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    for (const QAudioDevice &device : audio::inputs())
        m_device->addItem(device.description(), audio::deviceId(device));
    const QString wantedInput = settings.inputDevice.isEmpty()
        ? audio::deviceId(audio::findInput(QString()))
        : settings.inputDevice;
    const int inputIndex = m_device->findData(wantedInput);
    if (inputIndex >= 0)
        m_device->setCurrentIndex(inputIndex);

    m_rate = new QComboBox;
    for (const int rate : {16000, 24000, 44100, 48000})
        m_rate->addItem(QStringLiteral("%1 Hz").arg(rate), rate);
    m_rate->setCurrentIndex(qMax(0, m_rate->findData(settings.recordSampleRate)));

    m_channels = new QComboBox;
    m_channels->addItem(QStringLiteral("单声道"), 1);
    m_channels->addItem(QStringLiteral("双声道"), 2);
    m_channels->setCurrentIndex(settings.recordChannels == 2 ? 1 : 0);

    m_bits = new QComboBox;
    m_bits->addItem(QStringLiteral("16-bit PCM"), 16);
    m_bits->addItem(QStringLiteral("24-bit PCM"), 24);
    m_bits->addItem(QStringLiteral("32-bit PCM"), 32);
    m_bits->setCurrentIndex(qMax(0, m_bits->findData(settings.recordBits)));

    m_monitor = new AudioMonitor;
    m_monitor->setFixedHeight(230);
    m_monitor->setSampleRate(settings.recordSampleRate);
    m_monitor->setMode(AudioMonitor::Mode::Spectrogram);
    m_monitor->setCaption(QStringLiteral("等待录音…"));

    m_level = new QProgressBar;
    m_level->setRange(0, 100);
    m_level->setValue(0);
    m_level->setTextVisible(false);
    m_level->setFixedWidth(190);
    m_levelText = new QLabel(QStringLiteral("−∞ dB"));
    m_levelText->setMinimumWidth(62);
    m_elapsed = new QLabel(QStringLiteral("00:00"));
    m_elapsed->setMinimumWidth(52);
    m_size = new QLabel(QStringLiteral("0 KB"));
    m_size->setMinimumWidth(60);
    m_size->setForegroundRole(QPalette::PlaceholderText);

    m_status = new QLabel(QStringLiteral("选择格式后点「开始录音」。停止时会自动存入录音库。"));
    m_status->setWordWrap(true);
    m_status->setForegroundRole(QPalette::PlaceholderText);

    auto *formatRow = new QWidget;
    auto *formatLayout = new QHBoxLayout(formatRow);
    formatLayout->setContentsMargins(0, 0, 0, 0);
    formatLayout->setSpacing(8);
    formatLayout->addWidget(m_rate);
    formatLayout->addWidget(m_channels);
    formatLayout->addWidget(m_bits);
    formatLayout->addStretch(1);

    auto *deviceRow = new QHBoxLayout;
    deviceRow->addWidget(new QLabel(QStringLiteral("输入设备")));
    deviceRow->addWidget(m_device, 1);

    auto *meterRow = new QHBoxLayout;
    meterRow->addWidget(new QLabel(QStringLiteral("电平")));
    meterRow->addWidget(m_level);
    meterRow->addWidget(m_levelText);
    meterRow->addSpacing(14);
    meterRow->addWidget(new QLabel(QStringLiteral("时长")));
    meterRow->addWidget(m_elapsed);
    meterRow->addSpacing(10);
    meterRow->addWidget(m_size);
    meterRow->addStretch(1);

    m_recordButton = new QPushButton(theme::icon(QStringLiteral("record")),
                                     QStringLiteral("开始录音"));
    m_recordButton->setObjectName(QStringLiteral("recordButton"));
    m_recordButton->setMinimumHeight(34);
    auto *folderButton = new QPushButton(theme::icon(QStringLiteral("folder")),
                                         QStringLiteral("录音库"));
    auto *closeButton = new QPushButton(QStringLiteral("关闭"));
    for (QPushButton *button : {m_recordButton, folderButton, closeButton})
        button->setFixedHeight(34);
    m_recordButton->setIconSize(QSize(16, 16));
    folderButton->setIconSize(QSize(16, 16));
    m_recordButton->setMinimumWidth(110);
    folderButton->setMinimumWidth(96);

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_recordButton);
    buttons->addWidget(folderButton);
    buttons->addStretch(1);
    buttons->addWidget(closeButton);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 16);
    layout->setSpacing(12);
    layout->addLayout(deviceRow);
    layout->addWidget(formatRow);
    layout->addWidget(m_monitor, 1);
    layout->addLayout(meterRow);
    layout->addWidget(m_status);
    layout->addLayout(buttons);

    m_timer = new QTimer(this);
    m_timer->setInterval(200);
    connect(m_timer, &QTimer::timeout, this, &RecordDialog::updateElapsed);
    connect(m_recordButton, &QPushButton::clicked, this, [this] {
        if (m_recorder.isRunning())
            stopRecording();
        else
            startRecording();
    });
    connect(folderButton, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_store->directory()));
    });
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);

    connect(&m_recorder, &AudioRecorder::monitorChunk, this,
            [this](const QByteArray &pcm, int) { m_monitor->feed(pcm); });
    connect(&m_recorder, &AudioRecorder::level, this, [this](qreal rms, qreal) {
        const int value = rms <= 1e-6
            ? 0
            : qBound(0, int((20.0 * std::log10(rms) + 60.0) / 60.0 * 100.0), 100);
        m_level->setValue(value);
        m_levelText->setText(dbText(rms));
    });
    connect(&m_recorder, &AudioRecorder::stopped, this, [this] {
        if (m_timer->isActive()) {
            m_timer->stop();
            updateElapsed();
            setRecordingUi(false);
            saveRecording();
        }
    });
    connect(&m_recorder, &AudioRecorder::limitReached, this, [this] {
        m_status->setText(QStringLiteral("已达到录音长度/内存上限，自动停止。"));
    });
    connect(&m_recorder, &AudioRecorder::errorOccurred, this, [this](const QString &message) {
        m_status->setText(message);
    });
}

RecordDialog::~RecordDialog()
{
    if (m_recorder.isRunning())
        m_recorder.stop();
}

void RecordDialog::closeEvent(QCloseEvent *event)
{
    if (m_recorder.isRunning())
        stopRecording();
    QDialog::closeEvent(event);
}

void RecordDialog::applyFormat()
{
    m_settings.recordSampleRate = m_rate->currentData().toInt();
    m_settings.recordChannels = m_channels->currentData().toInt();
    m_settings.recordBits = m_bits->currentData().toInt();
    m_settings.save();
}

void RecordDialog::setRecordingUi(bool recording)
{
    m_rate->setEnabled(!recording);
    m_channels->setEnabled(!recording);
    m_bits->setEnabled(!recording);
    m_device->setEnabled(!recording);
    m_recordButton->setText(recording ? QStringLiteral("停止录音")
                                      : QStringLiteral("开始录音"));
    m_recordButton->setIcon(theme::icon(recording ? QStringLiteral("stop")
                                                  : QStringLiteral("record")));
}

void RecordDialog::startRecording()
{
    if (m_recorder.isRunning())
        return;

    applyFormat();
    const QAudioDevice device = audio::findInput(m_device->currentData().toString());
    const int rate = m_rate->currentData().toInt();
    const int channels = m_channels->currentData().toInt();
    const int bits = m_bits->currentData().toInt();

    QString error;
    if (!m_recorder.start(device, rate, channels, bits, &error)) {
        QMessageBox::warning(this, QStringLiteral("无法开始录音"), error);
        m_status->setText(error);
        return;
    }

    m_monitor->setSampleRate(rate);
    m_monitor->setMode(AudioMonitor::Mode::Spectrogram);
    m_monitor->clear();
    m_monitor->setCaption(QStringLiteral("录制 · %1 · %2 Hz · %3 声道 · %4-bit")
                              .arg(audio::displayName(device))
                              .arg(rate)
                              .arg(channels)
                              .arg(bits));
    m_level->setValue(0);
    m_levelText->setText(QStringLiteral("−∞ dB"));
    m_elapsed->setText(QStringLiteral("00:00"));
    m_size->setText(QStringLiteral("0 KB"));
    m_status->setText(QStringLiteral("正在录音…"));
    setRecordingUi(true);
    m_timer->start();
}

void RecordDialog::stopRecording()
{
    if (!m_recorder.isRunning())
        return;
    m_status->setText(QStringLiteral("正在保存…"));
    m_recorder.stop();
}

void RecordDialog::updateElapsed()
{
    const qint64 ms = m_recorder.durationMs();
    m_elapsed->setText(durationText(ms));
    m_size->setText(sizeText(m_recorder.byteSize()));
}

void RecordDialog::saveRecording()
{
    const qint64 minimumBytes = qint64(m_recorder.format().sampleRate()) * 2;
    if (m_recorder.byteSize() < minimumBytes) {
        m_status->setText(QStringLiteral("录音太短，没有保存。"));
        return;
    }

    const int outputBits = m_bits->currentData().toInt();
    const QByteArray wav = m_recorder.wavData(outputBits);
    if (wav.isEmpty()) {
        m_status->setText(QStringLiteral("没有可保存的音频。"));
        return;
    }

    RecordingInfo info;
    info.title = m_store->nextTitle();
    info.createdAt = QDateTime::currentDateTime();
    info.durationMs = m_recorder.durationMs();
    info.sampleRate = m_recorder.format().sampleRate();
    info.channels = m_recorder.format().channelCount();
    info.bits = outputBits;
    info.peaks = m_recorder.peaks(180);

    QString error;
    const QString id = m_store->addWav(wav, info, &error);
    if (id.isEmpty()) {
        m_status->setText(error);
        QMessageBox::warning(this, QStringLiteral("保存录音失败"), error);
        return;
    }

    m_status->setText(QStringLiteral("已保存到录音库 · %1 · %2")
                          .arg(durationText(info.durationMs), sizeText(wav.size())));
    emit saved(id);
}
