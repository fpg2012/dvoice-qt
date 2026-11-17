#include "preferences.h"

#include "core/audiodevices.h"
#include "theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QIcon>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QSlider>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace {

QLabel *hint(const QString &text)
{
    auto *label = new QLabel(text);
    label->setWordWrap(true);
    label->setForegroundRole(QPalette::PlaceholderText);
    return label;
}

void fillDevices(QComboBox *combo, const QList<QAudioDevice> &devices, const QString &selected)
{
    combo->clear();
    combo->addItem(QStringLiteral("系统默认"), QString());
    int match = 0;
    for (const QAudioDevice &device : devices) {
        const QString id = audio::deviceId(device);
        combo->addItem(device.description(), id);
        if (!selected.isEmpty() && id == selected)
            match = combo->count() - 1;
    }
    combo->setCurrentIndex(match);
}

} // namespace

PreferencesDialog::PreferencesDialog(const Settings &settings, QWidget *parent)
    : QDialog(parent)
    , m_result(settings)
{
    setWindowTitle(QStringLiteral("首选项"));
    setModal(true);
    buildUi();
    load(settings);
    resize(560, 460);
}

void PreferencesDialog::buildUi()
{
    m_tabs = new QTabWidget(this);

    // 接口
    auto *apiPage = new QWidget;
    auto *apiForm = new QFormLayout(apiPage);
    apiForm->setContentsMargins(18, 18, 18, 18);
    apiForm->setSpacing(10);

    auto *keyRow = new QWidget;
    auto *keyLayout = new QHBoxLayout(keyRow);
    keyLayout->setContentsMargins(0, 0, 0, 0);
    keyLayout->setSpacing(6);
    m_key = new QLineEdit;
    m_key->setEchoMode(QLineEdit::Password);
    m_key->setPlaceholderText(QStringLiteral("留空则使用环境变量或 DSH 凭据"));
    auto *fromEnv = new QPushButton(QStringLiteral("读取"));
    fromEnv->setToolTip(QStringLiteral("从环境变量或 ~/.dsh/.credentials.yaml 读取"));
    keyLayout->addWidget(m_key, 1);
    keyLayout->addWidget(fromEnv);
    apiForm->addRow(QStringLiteral("API Key"), keyRow);

    m_showKey = new QCheckBox(QStringLiteral("显示密钥"));
    apiForm->addRow(QString(), m_showKey);
    m_remember = new QCheckBox(QStringLiteral("记住密钥（明文保存在本机配置中）"));
    apiForm->addRow(QString(), m_remember);
    m_baseUrl = new QLineEdit;
    m_baseUrl->setPlaceholderText(QStringLiteral("https://api.deepseek.com"));
    apiForm->addRow(QStringLiteral("API 地址"), m_baseUrl);
    apiForm->addRow(QString(), hint(QStringLiteral(
        "语音识别与语音合成都使用这个地址。密钥只用于请求 Authorization 头，不会发送到其他位置。")));

    connect(m_showKey, &QCheckBox::toggled, this, [this](bool visible) {
        m_key->setEchoMode(visible ? QLineEdit::Normal : QLineEdit::Password);
    });
    connect(fromEnv, &QPushButton::clicked, this, [this] {
        const QString key = Settings::discoverApiKey();
        if (key.isEmpty()) {
            QMessageBox::information(this, QStringLiteral("没有找到密钥"),
                                     QStringLiteral("环境变量 DEEPSEEK_API_KEY 和 "
                                                    "~/.dsh/.credentials.yaml 里都没有可用的密钥。"));
            return;
        }
        m_key->setText(key);
        m_key->setEchoMode(QLineEdit::Password);
        m_showKey->setChecked(false);
    });
    m_tabs->addTab(apiPage, QStringLiteral("接口"));

    // 识别
    auto *asrPage = new QWidget;
    auto *asrForm = new QFormLayout(asrPage);
    asrForm->setContentsMargins(18, 18, 18, 18);
    asrForm->setSpacing(10);
    auto sliderRow = [](QSlider *slider, QLabel *value) {
        slider->setOrientation(Qt::Horizontal);
        value->setMinimumWidth(64);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto *row = new QWidget;
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(10);
        layout->addWidget(slider, 1);
        layout->addWidget(value);
        return row;
    };

    m_silence = new QSlider;
    m_silence->setRange(300, 1500);
    m_silence->setSingleStep(50);
    m_silence->setPageStep(100);
    m_silenceValue = new QLabel;
    connect(m_silence, &QSlider::valueChanged, this, [this](int value) {
        m_silenceValue->setText(QStringLiteral("%1 ms").arg(value));
    });
    asrForm->addRow(QStringLiteral("停顿断句"), sliderRow(m_silence, m_silenceValue));

    m_minSpeech = new QSlider;
    m_minSpeech->setRange(150, 800);
    m_minSpeech->setSingleStep(50);
    m_minSpeech->setPageStep(100);
    m_minSpeechValue = new QLabel;
    connect(m_minSpeech, &QSlider::valueChanged, this, [this](int value) {
        m_minSpeechValue->setText(QStringLiteral("%1 ms").arg(value));
    });
    asrForm->addRow(QStringLiteral("最短语音"), sliderRow(m_minSpeech, m_minSpeechValue));

    m_maxSegment = new QSlider;
    m_maxSegment->setRange(5, 30);
    m_maxSegment->setSingleStep(1);
    m_maxSegment->setPageStep(5);
    m_maxSegmentValue = new QLabel;
    connect(m_maxSegment, &QSlider::valueChanged, this, [this](int value) {
        m_maxSegmentValue->setText(QStringLiteral("%1 s").arg(value));
    });
    asrForm->addRow(QStringLiteral("单段上限"), sliderRow(m_maxSegment, m_maxSegmentValue));
    asrForm->addRow(QString(), hint(QStringLiteral(
        "停顿超过设定时长就切出一段音频上传识别，文本按顺序追加。"
        "服务端一次最多接受 120 秒、8 MiB，单段上限建议保持在 15 秒左右以降低延迟。")));
    m_tabs->addTab(asrPage, QStringLiteral("识别"));

    // 合成
    auto *ttsPage = new QWidget;
    auto *ttsForm = new QFormLayout(ttsPage);
    ttsForm->setContentsMargins(18, 18, 18, 18);
    ttsForm->setSpacing(10);
    m_voice = new QComboBox;
    m_voice->addItem(QStringLiteral("stella · 海星（女声）"), QStringLiteral("stella"));
    m_voice->addItem(QStringLiteral("tide · 暗潮（男声）"), QStringLiteral("tide"));
    ttsForm->addRow(QStringLiteral("音色"), m_voice);
    m_filterMarkdown = new QCheckBox(QStringLiteral("朗读前去除 Markdown 标记"));
    ttsForm->addRow(QString(), m_filterMarkdown);
    ttsForm->addRow(QString(), hint(QStringLiteral(
        "合成结果按 24 kHz / 16-bit / 单声道边下载边播放；长文本会自动按句切分。"
        "接口不支持调节语速。")));
    m_tabs->addTab(ttsPage, QStringLiteral("合成"));

    // 音频设备
    auto *audioPage = new QWidget;
    auto *audioForm = new QFormLayout(audioPage);
    audioForm->setContentsMargins(18, 18, 18, 18);
    audioForm->setSpacing(10);
    m_input = new QComboBox;
    m_output = new QComboBox;
    for (QComboBox *combo : {m_input, m_output}) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(26);
        combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    m_refresh = new QPushButton(QStringLiteral("刷新设备"));
    audioForm->addRow(QStringLiteral("输入设备"), m_input);
    audioForm->addRow(QStringLiteral("输出设备"), m_output);
    audioForm->addRow(QString(), m_refresh);
    audioForm->addRow(QString(), hint(QStringLiteral(
        "设备变更在下次开始录音或播放时生效。录音固定使用 16 kHz 单声道，"
        "播放固定使用 24 kHz 单声道，由系统音频服务负责转换。")));
    connect(m_refresh, &QPushButton::clicked, this, [this] {
        const QString input = m_input->currentData().toString();
        const QString output = m_output->currentData().toString();
        fillDevices(m_input, audio::inputs(), input);
        fillDevices(m_output, audio::outputs(), output);
    });
    m_tabs->addTab(audioPage, QStringLiteral("音频"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    auto *reset = buttons->addButton(QStringLiteral("恢复默认"), QDialogButtonBox::ResetRole);
    connect(reset, &QPushButton::clicked, this, [this] {
        Settings defaults = Settings::defaults();
        defaults.apiKey = m_result.apiKey;
        defaults.rememberApiKey = m_result.rememberApiKey;
        load(defaults);
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &PreferencesDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);
    layout->addWidget(m_tabs, 1);
    layout->addWidget(buttons);
}

void PreferencesDialog::load(const Settings &settings)
{
    m_key->setText(settings.apiKey);
    m_remember->setChecked(settings.rememberApiKey);
    m_baseUrl->setText(settings.baseUrl);
    fillDevices(m_input, audio::inputs(), settings.inputDevice);
    fillDevices(m_output, audio::outputs(), settings.outputDevice);
    const int voiceIndex = m_voice->findData(settings.voiceId);
    if (voiceIndex >= 0)
        m_voice->setCurrentIndex(voiceIndex);
    m_filterMarkdown->setChecked(settings.filterMarkdown);
    m_silence->setValue(qBound(300, settings.silenceMs, 1500));
    m_minSpeech->setValue(qBound(150, settings.minSpeechMs, 800));
    m_maxSegment->setValue(qBound(5, qRound(settings.maxSegmentMs / 1000.0), 30));
}

void PreferencesDialog::selectTab(int index)
{
    if (m_tabs && index >= 0 && index < m_tabs->count())
        m_tabs->setCurrentIndex(index);
}

void PreferencesDialog::accept()
{
    const QString base = m_baseUrl->text().trimmed();
    const QUrl url(base);
    if (base.isEmpty() || !url.isValid() || url.host().isEmpty()
        || (url.scheme() != QLatin1String("https") && url.scheme() != QLatin1String("http"))) {
        QMessageBox::warning(this, QStringLiteral("API 地址无效"),
                             QStringLiteral("请输入完整的 http:// 或 https:// 地址。"));
        return;
    }

    m_result.apiKey = m_key->text().trimmed();
    m_result.rememberApiKey = m_remember->isChecked();
    m_result.baseUrl = base;
    m_result.inputDevice = m_input->currentData().toString();
    m_result.outputDevice = m_output->currentData().toString();
    m_result.voiceId = m_voice->currentData().toString();
    m_result.filterMarkdown = m_filterMarkdown->isChecked();
    const auto roundToStep = [](int value) { return ((value + 25) / 50) * 50; };
    m_result.silenceMs = roundToStep(m_silence->value());
    m_result.minSpeechMs = roundToStep(m_minSpeech->value());
    m_result.maxSegmentMs = m_maxSegment->value() * 1000;
    QDialog::accept();
}

void showAboutDialog(QWidget *parent)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("关于 dvoice-qt"));
    dialog.setModal(true);
    dialog.resize(500, 450);

    auto *icon = new QLabel;
    icon->setPixmap(theme::appIcon().pixmap(64, 64));
    icon->setFixedSize(64, 64);

    QFont titleFont = dialog.font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.45);
    titleFont.setBold(true);
    auto *title = new QLabel(QStringLiteral("dvoice-qt %1").arg(QStringLiteral(DV_VERSION)));
    title->setFont(titleFont);
    auto *subtitle = new QLabel(QStringLiteral("DeepSeek 语音识别与语音合成桌面客户端"));
    subtitle->setForegroundRole(QPalette::PlaceholderText);

    auto *headerText = new QVBoxLayout;
    headerText->setSpacing(2);
    headerText->addStretch(1);
    headerText->addWidget(title);
    headerText->addWidget(subtitle);
    headerText->addStretch(1);

    auto *header = new QHBoxLayout;
    header->addWidget(icon, 0, Qt::AlignTop);
    header->addSpacing(16);
    header->addLayout(headerText, 1);

    auto *description = new QLabel(QStringLiteral(
        "识别：麦克风停顿断句，也可打开或拖入音频文件。\n"
        "录音：16/24/32 位 WAV，增益可调，自动保存到录音库。\n"
        "回放：选中展开波形，双击详情，可播放选区。\n"
        "合成：边合成边播放，整段下载完可以拖动定位。"));
    description->setWordWrap(true);

    auto *separator = new QFrame;
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Plain);

    QFont sectionFont = dialog.font();
    sectionFont.setBold(true);

    auto *shortcutTitle = new QLabel(QStringLiteral("快捷键"));
    shortcutTitle->setFont(sectionFont);

    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(22);
    grid->setVerticalSpacing(7);
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    const QVector<QPair<QString, QString>> shortcuts = {
        {QStringLiteral("Ctrl+R"), QStringLiteral("录制并转写")},
        {QStringLiteral("Ctrl+O"), QStringLiteral("打开音频文件")},
        {QStringLiteral("Ctrl+Shift+R"), QStringLiteral("录音到录音库")},
        {QStringLiteral("Ctrl+Enter"), QStringLiteral("朗读选中文本或全文")},
        {QStringLiteral("Ctrl+Shift+Enter"), QStringLiteral("重新请求 TTS 合成")},
        {QStringLiteral("Ctrl+P"), QStringLiteral("播放 / 暂停")},
        {QStringLiteral("Ctrl+,"), QStringLiteral("首选项")},
    };
    int row = 0;
    for (const auto &entry : shortcuts) {
        auto *key = new QLabel(entry.first);
        key->setFont(mono);
        key->setForegroundRole(QPalette::PlaceholderText);
        grid->addWidget(key, row, 0);
        grid->addWidget(new QLabel(entry.second), row, 1);
        ++row;
    }
    grid->setColumnStretch(1, 1);

    auto *endpointTitle = new QLabel(QStringLiteral("接口"));
    endpointTitle->setFont(sectionFont);

    auto *endpoints = new QLabel(QStringLiteral(
        "ASR  /v1/tools/asr      TTS  /v1/tools/tts\n"
        "api.deepseek.com · Qt 6 / CMake / PipeWire"));
    endpoints->setFont(mono);
    endpoints->setForegroundRole(QPalette::PlaceholderText);
    endpoints->setWordWrap(true);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    buttons->button(QDialogButtonBox::Close)->setIcon(QIcon());
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 22, 24, 18);
    layout->setSpacing(14);
    layout->addLayout(header);
    layout->addWidget(description);
    layout->addWidget(separator);
    layout->addWidget(shortcutTitle);
    layout->addLayout(grid);
    layout->addWidget(endpointTitle);
    layout->addWidget(endpoints);
    layout->addStretch(1);
    layout->addWidget(buttons);

    dialog.exec();
}
