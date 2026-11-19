#include "devicetest.h"

#include "audiomonitor.h"
#include "core/audiodevices.h"
#include "core/pcm.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QBuffer>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QIcon>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>
#include <QVector>
#include <QtEndian>

#include <cmath>
#include <cstring>

namespace {

QByteArray makeTestTone(int sampleRate)
{
    const double notes[] = {523.25, 659.25, 783.99};
    const int noteSamples = sampleRate * 3 / 5; // 0.6 s
    const int fade = sampleRate / 50;           // 20 ms
    QByteArray pcm;
    pcm.reserve(noteSamples * 3 * 2);
    for (const double note : notes) {
        for (int i = 0; i < noteSamples; ++i) {
            double envelope = 1.0;
            if (i < fade)
                envelope = double(i) / fade;
            else if (i > noteSamples - fade)
                envelope = double(noteSamples - i) / fade;
            const double sample = std::sin(2.0 * M_PI * note * i / sampleRate) * 0.22 * envelope;
            const qint16 value = qint16(sample * 32767.0);
            char bytes[2];
            qToLittleEndian(value, bytes);
            pcm.append(bytes, 2);
        }
    }
    return pcm;
}

// 把 24 kHz 单声道 16-bit 的测试音转成目标设备真正支持的格式。
QByteArray toDevicePcm(const QByteArray &mono16, int sourceRate, const QAudioFormat &target)
{
    QAudioFormat source;
    source.setSampleRate(sourceRate);
    source.setChannelCount(1);
    source.setSampleFormat(QAudioFormat::Int16);
    if (target == source)
        return mono16;

    QVector<float> samples = pcm::resample(pcm::toMonoFloat(mono16, source), sourceRate,
                                           target.sampleRate());
    const int channels = qMax(1, target.channelCount());
    QByteArray out;
    out.reserve(int(samples.size()) * channels * target.bytesPerSample());
    for (const float value : samples) {
        const float clamped = qBound(-1.0f, value, 1.0f);
        for (int channel = 0; channel < channels; ++channel) {
            char buffer[4];
            switch (target.sampleFormat()) {
            case QAudioFormat::Float: {
                std::memcpy(buffer, &clamped, sizeof(float));
                out.append(buffer, 4);
                break;
            }
            case QAudioFormat::Int32: {
                qToLittleEndian(qint32(std::llround(double(clamped) * 2147483647.0)), buffer);
                out.append(buffer, 4);
                break;
            }
            default: {
                qToLittleEndian(qint16(std::lround(clamped * 32767.0f)), buffer);
                out.append(buffer, 2);
                break;
            }
            }
        }
    }
    return out;
}

QString dbText(qreal rms)
{
    if (rms <= 1e-6)
        return QStringLiteral("−∞ dB");
    return QStringLiteral("%1 dB").arg(int(std::lround(20.0 * std::log10(rms))));
}

} // namespace

MicTestDialog::MicTestDialog(const Settings &settings, QWidget *parent)
    : QDialog(parent)
    , m_settings(settings)
{
    setWindowTitle(QStringLiteral("麦克风测试"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(660, 420);

    m_device = new QComboBox;
    m_device->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_device->setMinimumContentsLength(24);
    m_device->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    for (const QAudioDevice &device : audio::inputs()) {
        m_device->addItem(device.description(), audio::deviceId(device));
    }
    const QString wanted = settings.inputDevice.isEmpty()
        ? audio::deviceId(audio::findInput(QString()))
        : settings.inputDevice;
    const int index = m_device->findData(wanted);
    if (index >= 0)
        m_device->setCurrentIndex(index);

    m_monitor = new AudioMonitor;
    m_monitor->setSampleRate(16000);
    m_monitor->setMode(AudioMonitor::Mode::Bars);
    m_monitor->setCaption(QStringLiteral("输入 · 16 kHz · 单声道"));

    m_level = new QProgressBar;
    m_level->setRange(0, 100);
    m_level->setValue(0);
    m_level->setTextVisible(false);
    m_level->setFixedWidth(180);
    m_levelText = new QLabel(QStringLiteral("−∞ dB"));
    m_levelText->setMinimumWidth(64);

    m_status = new QLabel(QStringLiteral("对着麦克风说话，观察波形和电平。"));
    m_status->setWordWrap(true);

    auto *top = new QHBoxLayout;
    top->addWidget(new QLabel(QStringLiteral("输入设备")));
    top->addWidget(m_device, 1);

    auto *levelRow = new QHBoxLayout;
    levelRow->addWidget(new QLabel(QStringLiteral("电平")));
    levelRow->addWidget(m_level);
    levelRow->addWidget(m_levelText);
    levelRow->addStretch(1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    buttons->button(QDialogButtonBox::Close)->setIcon(QIcon());
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);
    layout->addLayout(top);
    layout->addWidget(m_monitor, 1);
    layout->addLayout(levelRow);
    layout->addWidget(m_status);
    layout->addWidget(buttons);

    connect(m_device, &QComboBox::currentIndexChanged, this, &MicTestDialog::restart);
    connect(&m_recorder, &Recorder::pcmChunk, m_monitor, &AudioMonitor::feed);
    connect(&m_recorder, &Recorder::level, this, [this](qreal rms, qreal peak) {
        Q_UNUSED(peak)
        const int value = rms <= 1e-6 ? 0 : qBound(0, int((20.0 * std::log10(rms) + 60.0) / 60.0 * 100.0), 100);
        m_level->setValue(value);
        m_levelText->setText(dbText(rms));
    });
    connect(&m_recorder, &Recorder::errorOccurred, this, [this](const QString &message) {
        m_status->setText(message);
    });

    restart();
}

MicTestDialog::~MicTestDialog()
{
    m_recorder.stop();
}

void MicTestDialog::restart()
{
    m_recorder.abort();
    m_monitor->clear();
    m_level->setValue(0);
    m_levelText->setText(QStringLiteral("−∞ dB"));

    if (m_device->count() == 0) {
        m_status->setText(QStringLiteral("没有可用的输入设备。"));
        return;
    }
    const QAudioDevice device = audio::findInput(m_device->currentData().toString());
    VadConfig config;
    config.silenceMs = 1000;
    QString error;
    if (!m_recorder.start(device, config, &error)) {
        m_status->setText(error);
        return;
    }
    m_status->setText(QStringLiteral("正在监听「%1」。").arg(device.description()));
}

SpeakerTestDialog::SpeakerTestDialog(const Settings &settings, QWidget *parent)
    : QDialog(parent)
    , m_settings(settings)
{
    setWindowTitle(QStringLiteral("扬声器测试"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(680, 500);

    m_device = new QComboBox;
    m_device->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_device->setMinimumContentsLength(24);
    m_device->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_monitor = new AudioMonitor;
    m_monitor->setSampleRate(24000);
    m_monitor->setMode(AudioMonitor::Mode::Spectrogram);
    m_monitor->setCaption(QStringLiteral("输出 · 24 kHz · 单声道 · 声谱图"));

    m_volume = new QSlider(Qt::Horizontal);
    m_volume->setRange(0, 100);
    m_volume->setValue(settings.volume);
    m_volume->setFixedWidth(160);

    auto *tone = new QPushButton(QStringLiteral("播放测试音"));
    auto *speech = new QPushButton(QStringLiteral("朗读测试语"));
    m_stop = new QPushButton(QStringLiteral("停止"));
    m_stop->setEnabled(false);

    m_status = new QLabel;
    m_status->setWordWrap(true);

    auto *top = new QHBoxLayout;
    top->addWidget(new QLabel(QStringLiteral("输出设备")));
    top->addWidget(m_device, 1);
    top->addSpacing(8);
    top->addWidget(new QLabel(QStringLiteral("音量")));
    top->addWidget(m_volume);

    auto *actions = new QHBoxLayout;
    actions->addWidget(tone);
    actions->addWidget(speech);
    actions->addWidget(m_stop);
    actions->addStretch(1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    buttons->button(QDialogButtonBox::Close)->setIcon(QIcon());
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);
    layout->addLayout(top);
    layout->addWidget(m_monitor, 1);
    layout->addLayout(actions);
    layout->addStretch(1);
    layout->addWidget(m_status);
    layout->addWidget(buttons);

    refreshDevices();
    m_status->setText(QStringLiteral("用测试音确认左右声道和音量，或朗读一句测试语。"));

    connect(m_device, &QComboBox::currentIndexChanged, this, [this] {
        stopAll();
        m_player.setOutputDevice(audio::findOutput(m_device->currentData().toString()));
    });
    connect(m_volume, &QSlider::valueChanged, this, &SpeakerTestDialog::applyVolume);
    connect(tone, &QPushButton::clicked, this, &SpeakerTestDialog::startTone);
    connect(speech, &QPushButton::clicked, this, &SpeakerTestDialog::startSpeech);
    connect(m_stop, &QPushButton::clicked, this, &SpeakerTestDialog::stopAll);

    connect(&m_player, &Player::audioChunk, m_monitor, &AudioMonitor::feed);
    connect(&m_player, &Player::stateChanged, this, [this](Player::State state) {
        if (state == Player::State::Buffering) {
            m_status->setText(QStringLiteral("正在合成测试语…"));
            m_stop->setEnabled(true);
        } else if (state == Player::State::Playing) {
            m_status->setText(QStringLiteral("正在播放测试语。"));
            m_stop->setEnabled(true);
        } else if (state == Player::State::Finished) {
            m_status->setText(QStringLiteral("测试语播放完成。"));
            m_stop->setEnabled(false);
        }
    });
    connect(&m_player, &Player::errorOccurred, this, [this](const QString &message) {
        m_status->setText(message);
        m_stop->setEnabled(false);
    });

    m_player.setOutputDevice(audio::findOutput(m_settings.outputDevice));
    m_player.setVolume(m_volume->value() / 100.0);
}

SpeakerTestDialog::~SpeakerTestDialog()
{
    stopAll();
}

void SpeakerTestDialog::refreshDevices()
{
    m_device->clear();
    for (const QAudioDevice &device : audio::outputs())
        m_device->addItem(device.description(), audio::deviceId(device));
    const QString wanted = m_settings.outputDevice.isEmpty()
        ? audio::deviceId(audio::findOutput(QString()))
        : m_settings.outputDevice;
    const int index = m_device->findData(wanted);
    if (index >= 0)
        m_device->setCurrentIndex(index);
}

void SpeakerTestDialog::applyVolume()
{
    m_player.setVolume(m_volume->value() / 100.0);
    if (m_toneSink)
        m_toneSink->setVolume(m_volume->value() / 100.0);
}

void SpeakerTestDialog::setStatus(const QString &text)
{
    m_status->setText(text);
}

void SpeakerTestDialog::startTone()
{
    stopAll();
    const QByteArray tone = makeTestTone(24000);
    if (tone.isEmpty())
        return;
    m_monitor->clear();
    m_monitor->feed(tone);

    const QAudioDevice device = audio::findOutput(m_device->currentData().toString());
    const audio::ResolvedFormat resolved = audio::resolveOutputFormat(device, 24000, 1, 16);
    if (!resolved.format.isValid()) {
        setStatus(QStringLiteral("「%1」没有可用的播放格式。").arg(audio::displayName(device)));
        return;
    }
    m_tone = toDevicePcm(tone, 24000, resolved.format);
    if (m_tone.isEmpty())
        return;

    m_toneSink = device.isNull() ? new QAudioSink(resolved.format, this)
                                 : new QAudioSink(device, resolved.format, this);
    m_toneSink->setVolume(m_volume->value() / 100.0);
    m_toneBuffer = new QBuffer(&m_tone, this);
    m_toneBuffer->open(QIODevice::ReadOnly);

    connect(m_toneSink, &QAudioSink::stateChanged, this, [this](QAudio::State state) {
        if ((state == QAudio::IdleState || state == QAudio::StoppedState)
            && m_toneBuffer && m_toneBuffer->atEnd()) {
            cleanupTone();
            setStatus(QStringLiteral("测试音播放完成。"));
            m_stop->setEnabled(false);
        }
    });
    m_toneSink->start(m_toneBuffer);
    m_stop->setEnabled(true);
    setStatus(QStringLiteral("正在播放测试音（三个上行音）。"));
}

void SpeakerTestDialog::startSpeech()
{
    stopAll();
    m_monitor->clear();
    m_monitor->setMode(AudioMonitor::Mode::Spectrogram);
    m_monitor->setCaption(QStringLiteral("输出 · 24 kHz · 单声道 · 声谱图"));
    tts::Request request;
    request.baseUrl = m_settings.endpoint();
    request.apiKey = m_settings.effectiveApiKey();
    request.voiceId = m_settings.voiceId;
    request.filterMarkdown = false;
    m_player.setOutputDevice(audio::findOutput(m_device->currentData().toString()));
    m_player.play(QStringLiteral("你好，这是 dvoice 的扬声器测试。"), request);
}

void SpeakerTestDialog::stopAll()
{
    m_player.stop();
    cleanupTone();
    m_stop->setEnabled(false);
}

void SpeakerTestDialog::cleanupTone()
{
    if (m_toneSink) {
        m_toneSink->disconnect(this);
        m_toneSink->stop();
        m_toneSink->deleteLater();
        m_toneSink = nullptr;
    }
    if (m_toneBuffer) {
        m_toneBuffer->close();
        m_toneBuffer->deleteLater();
        m_toneBuffer = nullptr;
    }
}
