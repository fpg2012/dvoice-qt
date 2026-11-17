#include "mainwindow.h"

#include "audiomonitor.h"
#include "core/audiofile.h"
#include "core/audiodevices.h"
#include "core/pcm.h"
#include "core/recordingstore.h"
#include "core/wav.h"
#include "devicetest.h"
#include "librarypage.h"
#include "preferences.h"
#include "theme.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QFileInfo>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QRandomGenerator>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QUrl>
#include <QtEndian>

#include <cmath>

namespace {

QString formatTime(qint64 ms)
{
    if (ms < 0)
        ms = 0;
    const qint64 seconds = ms / 1000;
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    const qint64 secs = seconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(secs, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(secs, 2, 10, QLatin1Char('0'));
}

QString shortcutText(const QString &text)
{
    return text;
}

QByteArray demoAudio(double &phase, int samples)
{
    QByteArray pcm;
    pcm.reserve(samples * 2);
    for (int i = 0; i < samples; ++i) {
        const double t = (phase + i) / 24000.0;
        const double phrasePos = std::fmod(t, 3.4);
        double gate = 1.0;
        if (phrasePos < 0.45) {
            gate = 0.02;
        } else if (phrasePos < 0.75) {
            gate = 0.02 + 0.98 * (phrasePos - 0.45) / 0.30;
        } else if (phrasePos > 3.15) {
            gate = 0.02 + 0.98 * (3.4 - phrasePos) / 0.25;
        }
        const double syllable = 0.55 + 0.45 * std::sin(2.0 * M_PI * 4.6 * t);
        const double noise =
            (QRandomGenerator::global()->generateDouble() * 2.0 - 1.0) * 0.018;
        const double value =
            (std::sin(2.0 * M_PI * 140.0 * t) * 0.30
             + std::sin(2.0 * M_PI * 280.0 * t) * 0.14
             + std::sin(2.0 * M_PI * 700.0 * t) * 0.07
             + std::sin(2.0 * M_PI * 1200.0 * t) * 0.03)
                * gate * syllable
            + noise * gate;
        const qint16 sample = qint16(qBound(-1.0, value, 1.0) * 32767.0);
        char bytes[2];
        qToLittleEndian(sample, bytes);
        pcm.append(bytes, 2);
    }
    phase += samples;
    return pcm;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("dvoice"));
    setWindowIcon(theme::appIcon());
    setMinimumSize(700, 480);

    m_settings = Settings::load();
    m_store = new RecordingStore(this);

    buildActions();
    buildUi();
    buildMenus();
    connectSignals();

    m_demoTimer = new QTimer(this);
    m_demoTimer->setInterval(50);
    connect(m_demoTimer, &QTimer::timeout, this, [this] {
        m_monitor->feed(demoAudio(m_demoPhase, 1200)); // 50 ms @ 24 kHz
    });

    m_asr.configure(m_settings.endpoint(), m_settings.effectiveApiKey());
    m_player.setOutputDevice(outputDevice());
    m_player.setVolume(m_settings.volume / 100.0);
    {
        QSignalBlocker blocker(m_volume);
        m_volume->setValue(m_settings.volume);
    }

    QSettings store;
    if (store.value(QStringLiteral("window/layout")).toInt() >= 2) {
        if (!restoreGeometry(store.value(QStringLiteral("window/geometry")).toByteArray()))
            resize(860, 600);
    } else {
        resize(860, 600);
    }

    refreshIcons();
    updateCost();
    setStatus(QStringLiteral("就绪"));
    updatePlayAction();
    m_stopAction->setEnabled(false);
    m_library->applySettings(m_settings);
    onTabChanged(m_tabs->currentIndex());
}

MainWindow::~MainWindow()
{
    m_recorder.abort();
    m_player.stop();
}

void MainWindow::buildActions()
{
    m_recordAction = new QAction(theme::icon(QStringLiteral("mic")), QStringLiteral("录制并转写"), this);
    m_recordAction->setCheckable(true);
    m_recordAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
    m_recordAction->setToolTip(shortcutText(QStringLiteral("开始录音 (Ctrl+R)")));

    m_stopAction = new QAction(theme::icon(QStringLiteral("stop")), QStringLiteral("停止"), this);
    m_stopAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+.")));
    m_stopAction->setToolTip(shortcutText(QStringLiteral("停止录音和播放 (Ctrl+.)")));

    m_speakAction = new QAction(theme::icon(QStringLiteral("speaker")), QStringLiteral("朗读"), this);
    m_speakAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Return")));
    m_speakAction->setToolTip(shortcutText(QStringLiteral("朗读选中文本或全文 (Ctrl+Enter)")));

    m_respeakAction = new QAction(theme::icon(QStringLiteral("refresh")),
                                  QStringLiteral("重新合成"), this);
    m_respeakAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+Return")));
    m_respeakAction->setToolTip(
        shortcutText(QStringLiteral("用当前文本重新请求 TTS 合成 (Ctrl+Shift+Enter)")));

    m_playAction = new QAction(theme::icon(QStringLiteral("play")), QStringLiteral("播放"), this);
    m_playAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+P")));
    m_playAction->setToolTip(shortcutText(QStringLiteral("播放 / 暂停 (Ctrl+P)")));

    m_settingsAction =
        new QAction(theme::icon(QStringLiteral("settings")), QStringLiteral("首选项"), this);
    m_settingsAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+,")));
    m_settingsAction->setToolTip(shortcutText(QStringLiteral("首选项 (Ctrl+,)")));
}

void MainWindow::buildUi()
{
    m_transcribePage = new QWidget(this);
    auto *central = m_transcribePage;
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(18, 12, 18, 8);
    layout->setSpacing(10);

    m_text = new QPlainTextEdit(central);
    m_text->setPlaceholderText(
        QStringLiteral("点击工具栏的麦克风开始录音转写；也可以粘贴文字后点扬声器朗读。"));
    m_text->setTabChangesFocus(true);
    m_text->setAcceptDrops(false);
    QFont textFont = m_text->font();
    textFont.setPointSizeF(textFont.pointSizeF() + 1.0);
    m_text->setFont(textFont);
    m_text->document()->setDocumentMargin(10);
    m_text->setMinimumHeight(140);
    layout->addWidget(m_text, 1);

    m_monitor = new AudioMonitor(central);
    m_monitor->setFixedHeight(190);
    layout->addWidget(m_monitor);

    auto *bar = new QWidget(central);
    auto *row = new QHBoxLayout(bar);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(10);

    auto *playButton = new QToolButton(bar);
    playButton->setObjectName(QStringLiteral("transportPlay"));
    playButton->setDefaultAction(m_playAction);
    playButton->setAutoRaise(false);
    playButton->setIconSize(QSize(20, 20));
    playButton->setFixedSize(38, 38);
    playButton->setToolButtonStyle(Qt::ToolButtonIconOnly);

    auto *respeakButton = new QToolButton(bar);
    respeakButton->setDefaultAction(m_respeakAction);
    respeakButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    respeakButton->setIconSize(QSize(16, 16));
    respeakButton->setFixedHeight(32);
    respeakButton->setAutoRaise(false);

    m_time = new QLabel(QStringLiteral("00:00 / --:--"), bar);
    m_time->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_time->setMinimumWidth(112);
    m_time->setAlignment(Qt::AlignCenter);

    m_progress = new QSlider(Qt::Horizontal, bar);
    m_progress->setRange(0, 1000);
    m_progress->setValue(0);
    m_progress->setEnabled(false);
    m_progress->setToolTip(QStringLiteral("合成完成后可以拖动定位"));

    m_volumeIcon = new QLabel(bar);
    m_volumeIcon->setPixmap(theme::icon(QStringLiteral("volume")).pixmap(16, 16));
    m_volumeIcon->setToolTip(QStringLiteral("音量"));

    m_volume = new QSlider(Qt::Horizontal, bar);
    m_volume->setRange(0, 100);
    m_volume->setFixedWidth(90);
    m_volume->setToolTip(QStringLiteral("播放音量"));

    row->addWidget(playButton);
    row->addWidget(respeakButton);
    row->addWidget(m_time);
    row->addWidget(m_progress, 1);
    row->addWidget(m_volumeIcon);
    row->addWidget(m_volume);
    layout->addWidget(bar);

    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("appTabs"));
    m_tabs->setDocumentMode(true);
    m_tabs->addTab(m_transcribePage, theme::icon(QStringLiteral("mic")),
                   QStringLiteral("录制 · 转写"));
    m_library = new LibraryPage(m_store, m_settings, this);
    m_tabs->addTab(m_library, theme::icon(QStringLiteral("record")),
                   QStringLiteral("录制 · 回放"));
    setCentralWidget(m_tabs);

    m_text->viewport()->installEventFilter(this);
    m_monitor->installEventFilter(this);
    m_transcribePage->installEventFilter(this);
    m_tabs->installEventFilter(this);

    m_toolbar = new QToolBar(m_transcribePage);
    layout->insertWidget(0, m_toolbar);
    m_toolbar->setMovable(false);
    m_toolbar->setFloatable(false);
    m_toolbar->setIconSize(QSize(22, 22));
    m_toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_toolbar->addAction(m_recordAction);
    m_toolbar->addAction(m_stopAction);
    m_toolbar->addSeparator();
    m_toolbar->addAction(m_speakAction);
    m_toolbar->addAction(m_playAction);
    auto *spacer = new QWidget(m_toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_toolbar->addWidget(spacer);
    m_toolbar->addAction(m_settingsAction);

    m_state = new QLabel(this);
    m_usage = new QLabel(this);
    m_state->setAlignment(Qt::AlignVCenter);
    m_usage->setAlignment(Qt::AlignVCenter);
    m_usage->setToolTip(QStringLiteral("按 ASR 0.39 元/小时、TTS 1.5 元/万计费字符估算"));
    statusBar()->addWidget(m_state, 1);
    statusBar()->addPermanentWidget(m_usage);
    statusBar()->setSizeGripEnabled(false);
    statusBar()->setMinimumHeight(28);

    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);
}

void MainWindow::buildMenus()
{
    menuBar()->setNativeMenuBar(false);
    auto *file = menuBar()->addMenu(QStringLiteral("文件"));
    auto *openAudio = file->addAction(theme::icon(QStringLiteral("import")),
                                      QStringLiteral("打开音频…"));
    openAudio->setShortcut(QKeySequence::Open);
    auto *openFolder = file->addAction(theme::icon(QStringLiteral("folder")),
                                       QStringLiteral("打开录音库文件夹"));
    file->addSeparator();
    auto *clear = file->addAction(theme::icon(QStringLiteral("clear")), QStringLiteral("清空文本"));
    clear->setShortcut(QKeySequence(QStringLiteral("Ctrl+L")));
    auto *exportAction = file->addAction(QStringLiteral("导出文本…"));
    exportAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+S")));
    file->addSeparator();
    auto *quit = file->addAction(QStringLiteral("退出"));
    quit->setShortcut(QKeySequence(QStringLiteral("Ctrl+Q")));

    connect(openAudio, &QAction::triggered, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("打开音频"), QDir::homePath(),
            audiofile::nameFilters().join(QStringLiteral(";;")));
        if (!path.isEmpty())
            openAudioFile(path);
    });
    connect(openFolder, &QAction::triggered, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_store->directory()));
    });
    connect(clear, &QAction::triggered, this, [this] {
        m_text->clear();
        m_recognizedMs = 0;
        m_ttsChars = 0;
        updateCost();
        setStatus(QStringLiteral("已清空文本"));
    });
    connect(exportAction, &QAction::triggered, this, &MainWindow::exportText);
    connect(quit, &QAction::triggered, this, &QWidget::close);

    auto *edit = menuBar()->addMenu(QStringLiteral("编辑"));
    struct EditEntry {
        QString text;
        QKeySequence shortcut;
        void (QPlainTextEdit::*slot)();
    };
    const EditEntry entries[] = {
        {QStringLiteral("撤销"), QKeySequence::Undo, &QPlainTextEdit::undo},
        {QStringLiteral("重做"), QKeySequence::Redo, &QPlainTextEdit::redo},
        {QStringLiteral("剪切"), QKeySequence::Cut, &QPlainTextEdit::cut},
        {QStringLiteral("复制"), QKeySequence::Copy, &QPlainTextEdit::copy},
        {QStringLiteral("粘贴"), QKeySequence::Paste, &QPlainTextEdit::paste},
        {QStringLiteral("全选"), QKeySequence::SelectAll, &QPlainTextEdit::selectAll},
    };
    for (const EditEntry &entry : entries) {
        QAction *action = edit->addAction(entry.text);
        action->setShortcut(entry.shortcut);
        connect(action, &QAction::triggered, this, [this, entry] {
            (m_text->*entry.slot)();
        });
    }

    auto *audio = menuBar()->addMenu(QStringLiteral("音频"));
    audio->addAction(m_recordAction);
    audio->addAction(m_stopAction);
    audio->addAction(m_speakAction);
    audio->addAction(m_respeakAction);
    audio->addAction(m_playAction);
    audio->addSeparator();
    auto *libraryRecord = audio->addAction(theme::icon(QStringLiteral("record")),
                                           QStringLiteral("录音到录音库…"));
    libraryRecord->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+R")));
    auto *libraryImport = audio->addAction(theme::icon(QStringLiteral("import")),
                                           QStringLiteral("导入音频到录音库…"));
    audio->addSeparator();
    auto *micTest = audio->addAction(QStringLiteral("测试麦克风…"));
    auto *speakerTest = audio->addAction(QStringLiteral("测试扬声器…"));
    audio->addSeparator();
    audio->addAction(m_settingsAction);

    connect(libraryRecord, &QAction::triggered, this, [this] {
        m_tabs->setCurrentIndex(1);
        m_library->startRecording();
    });
    connect(libraryImport, &QAction::triggered, this, [this] {
        m_tabs->setCurrentIndex(1);
        m_library->importAudio();
    });
    connect(micTest, &QAction::triggered, this, &MainWindow::openMicTest);
    connect(speakerTest, &QAction::triggered, this, &MainWindow::openSpeakerTest);

    auto *help = menuBar()->addMenu(QStringLiteral("帮助"));
    auto *about = help->addAction(theme::icon(QStringLiteral("info")),
                                  QStringLiteral("关于 dvoice-qt"));
    about->setShortcut(QKeySequence::HelpContents);
    connect(about, &QAction::triggered, this, &MainWindow::openAbout);
}

void MainWindow::connectSignals()
{
    connect(m_recordAction, &QAction::triggered, this, [this](bool checked) {
        if (checked)
            startRecording();
        else
            stopRecording();
    });
    connect(m_stopAction, &QAction::triggered, this, &MainWindow::stopEverything);
    connect(m_speakAction, &QAction::triggered, this, &MainWindow::speakText);
    connect(m_respeakAction, &QAction::triggered, this, &MainWindow::speakText);
    const auto syncRespeak = [this] { m_respeakAction->setEnabled(m_speakAction->isEnabled()); };
    connect(m_speakAction, &QAction::changed, this, syncRespeak);
    syncRespeak();
    connect(m_playAction, &QAction::triggered, this, &MainWindow::togglePlayback);
    connect(m_settingsAction, &QAction::triggered, this, [this] { openPreferences(); });

    connect(&m_recorder, &Recorder::pcmChunk, this, [this](const QByteArray &pcm) {
        m_monitor->feed(pcm);
        if (m_recorder.isRunning())
            m_dictationPcm.append(pcm);
    });
    connect(&m_recorder, &Recorder::segment, this,
            [this](const QByteArray &pcm, int startMs, int endMs) {
                m_asr.enqueue(pcm, 16000, startMs, endMs);
            });
    connect(&m_recorder, &Recorder::speechChanged, this, [this](bool speaking) {
        if (!m_recorder.isRunning())
            return;
        setStatus(speaking ? QStringLiteral("检测到语音…停顿后自动断句")
                           : QStringLiteral("聆听中…"));
    });
    connect(&m_recorder, &Recorder::errorOccurred, this, [this](const QString &message) {
        QMessageBox::warning(this, QStringLiteral("麦克风错误"), message);
        {
            QSignalBlocker blocker(m_recordAction);
            m_recordAction->setChecked(false);
        }
        m_speakAction->setEnabled(true);
        updatePlayAction();
        m_stopAction->setEnabled(false);
        setStatus(QStringLiteral("录音已停止"));
    });

    connect(&m_asr, &asr::Queue::started, this, [this](int index, int) {
        if (m_importMode) {
            setStatus(QStringLiteral("识别 %1 · 第 %2/%3 段…")
                          .arg(m_importName)
                          .arg(index + 1)
                          .arg(qMax(m_importTotal, index + 1)));
            return;
        }
        setStatus(QStringLiteral("正在识别第 %1 段…").arg(index + 1));
    });
    connect(&m_asr, &asr::Queue::recognized, this,
            [this](int, QString text, int durationMs, int, int) {
                appendTranscript(text);
                if (m_recorder.isRunning() || !m_sessionRecordingId.isEmpty())
                    m_sessionTranscript.append(text);
                m_recognizedMs += qMax(1, durationMs);
                updateCost();
                if (m_importMode) {
                    ++m_importDone;
                    setStatus(QStringLiteral("识别 %1 · %2/%3 段…")
                                  .arg(m_importName)
                                  .arg(m_importDone)
                                  .arg(m_importTotal));
                    return;
                }
                if (!m_recorder.isRunning() && !m_asr.busy()
                    && m_player.state() == Player::State::Idle)
                    setStatus(QStringLiteral("已停止"));
            });
    connect(&m_asr, &asr::Queue::failed, this, [this](int index, const QString &message) {
        m_asrError = QStringLiteral("第 %1 段识别失败：%2").arg(index + 1).arg(message);
        setStatus(m_asrError);
    });
    connect(&m_asr, &asr::Queue::idle, this, [this] {
        if (m_importMode) {
            finishImport();
            return;
        }
        if (m_recorder.isRunning()) {
            setStatus(QStringLiteral("聆听中…"));
            return;
        }
        if (!m_sessionRecordingId.isEmpty()) {
            const QString id = m_sessionRecordingId;
            m_sessionRecordingId.clear();
            if (!m_sessionTranscript.trimmed().isEmpty()) {
                for (const Recording &recording : m_store->list()) {
                    if (recording.id != id)
                        continue;
                    Recording updated = recording;
                    updated.title = recording.title;
                    updated.transcript = m_sessionTranscript.trimmed();
                    m_store->update(updated);
                    break;
                }
            }
            m_sessionTranscript.clear();
        }
        if (!m_asrError.isEmpty()) {
            setStatus(m_asrError);
            return;
        }
        if (m_player.state() == Player::State::Idle)
            setStatus(QStringLiteral("已停止"));
    });

    connect(&m_player, &Player::stateChanged, this, [this](Player::State state) {
        updatePlayAction();
        const bool active = state == Player::State::Buffering
            || state == Player::State::Playing || state == Player::State::Paused;
        m_stopAction->setEnabled(m_recorder.isRunning() || active);
        switch (state) {
        case Player::State::Buffering:
            setStatus(QStringLiteral("正在合成…"));
            break;
        case Player::State::Playing:
            setStatus(QStringLiteral("正在朗读…"));
            break;
        case Player::State::Paused:
            setStatus(QStringLiteral("已暂停"));
            break;
        case Player::State::Finished:
            setStatus(QStringLiteral("朗读完成"));
            break;
        case Player::State::Failed:
            setStatus(QStringLiteral("朗读失败"));
            break;
        case Player::State::Idle:
            if (!m_recorder.isRunning() && !m_asr.busy())
                setStatus(QStringLiteral("就绪"));
            break;
        }
    });
    connect(&m_player, &Player::audioChunk, m_monitor, &AudioMonitor::feed);
    connect(&m_player, &Player::positionChanged, this, &MainWindow::updateTransport);
    connect(&m_player, &Player::errorOccurred, this, [this](const QString &message) {
        QMessageBox::warning(this, QStringLiteral("语音合成失败"), message);
        setStatus(QStringLiteral("朗读失败：%1").arg(message));
    });

    connect(m_progress, &QSlider::sliderReleased, this, [this] {
        if (m_player.canSeek())
            m_player.seek(m_progress->value());
    });
    connect(m_progress, &QSlider::sliderMoved, this, [this](int value) {
        m_time->setText(formatTime(value) + QStringLiteral(" / ")
                        + (m_player.durationMs() > 0 ? formatTime(m_player.durationMs())
                                                     : QStringLiteral("--:--")));
    });
    connect(m_volume, &QSlider::valueChanged, this, [this](int value) {
        m_player.setVolume(value / 100.0);
        m_settings.volume = value;
    });
}

void MainWindow::setStatus(const QString &text)
{
    if (m_state)
        m_state->setText(text);
}

void MainWindow::updateCost()
{
    if (!m_usage)
        return;
    const double asrCost = double(m_recognizedMs) / 3600000.0 * 0.39;
    const double ttsCost = double(m_ttsChars) / 10000.0 * 1.5;
    m_usage->setText(QStringLiteral("ASR %1 s · 合成 %2 字符 · ≈¥%3")
                         .arg(m_recognizedMs / 1000.0, 0, 'f', 1)
                         .arg(m_ttsChars)
                         .arg(asrCost + ttsCost, 0, 'f', 3));
}

void MainWindow::updatePlayAction()
{
    if (!m_playAction)
        return;
    const Player::State state = m_player.state();
    bool enabled = true;
    if (state == Player::State::Buffering)
        enabled = false;
    if (m_recorder.isRunning() || m_importMode)
        enabled = false;

    switch (state) {
    case Player::State::Playing:
        m_playAction->setIcon(theme::icon(QStringLiteral("pause")));
        m_playAction->setToolTip(shortcutText(QStringLiteral("暂停 (Ctrl+P)")));
        break;
    case Player::State::Paused:
        m_playAction->setIcon(theme::icon(QStringLiteral("play")));
        m_playAction->setToolTip(shortcutText(QStringLiteral("继续播放 (Ctrl+P)")));
        break;
    case Player::State::Finished:
        m_playAction->setIcon(theme::icon(QStringLiteral("play")));
        m_playAction->setToolTip(shortcutText(QStringLiteral("从头播放 (Ctrl+P)")));
        break;
    case Player::State::Buffering:
        m_playAction->setIcon(theme::icon(QStringLiteral("play")));
        m_playAction->setToolTip(QStringLiteral("正在合成…"));
        break;
    default:
        m_playAction->setIcon(theme::icon(QStringLiteral("play")));
        m_playAction->setToolTip(shortcutText(QStringLiteral("朗读文本 (Ctrl+P)")));
        break;
    }
    m_playAction->setEnabled(enabled);
}

void MainWindow::updateTransport(qint64 position, qint64 duration, qint64 buffered)
{
    if (!m_time || !m_progress)
        return;
    m_time->setText(formatTime(position) + QStringLiteral(" / ")
                    + (duration > 0 ? formatTime(duration) : QStringLiteral("--:--")));

    if (m_player.canSeek()) {
        m_progress->setEnabled(true);
        if (!m_progress->isSliderDown()) {
            m_progress->setRange(0, int(qMax<qint64>(1, duration)));
            m_progress->setValue(int(qBound<qint64>(0, position, duration)));
        }
    } else {
        m_progress->setEnabled(false);
        m_progress->setRange(0, 1000);
        const qint64 total = qMax<qint64>(1, buffered);
        m_progress->setValue(int(qBound<qint64>(0LL, position * 1000 / total, 1000LL)));
    }
}

void MainWindow::refreshIcons()
{
    if (!m_recordAction)
        return;
    m_recordAction->setIcon(theme::icon(QStringLiteral("mic")));
    m_stopAction->setIcon(theme::icon(QStringLiteral("stop")));
    m_speakAction->setIcon(theme::icon(QStringLiteral("speaker")));
    m_settingsAction->setIcon(theme::icon(QStringLiteral("settings")));
    if (m_volumeIcon)
        m_volumeIcon->setPixmap(theme::icon(QStringLiteral("volume")).pixmap(16, 16));
    updatePlayAction();
}

QAudioDevice MainWindow::inputDevice() const
{
    return audio::findInput(m_settings.inputDevice);
}

QAudioDevice MainWindow::outputDevice() const
{
    return audio::findOutput(m_settings.outputDevice);
}

bool MainWindow::ensureApiKey(const QString &action)
{
    if (!m_settings.effectiveApiKey().isEmpty())
        return true;
    QMessageBox::warning(
        this, QStringLiteral("未配置 API Key"),
        QStringLiteral("无法%1：还没有可用的 DeepSeek API Key。\n\n"
                       "请打开「音频 → 首选项 → 接口」填写，或设置环境变量 "
                       "DEEPSEEK_API_KEY。\n当前也会读取 ~/.dsh/.credentials.yaml。")
            .arg(action));
    return false;
}

void MainWindow::appendTranscript(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return;

    QTextCursor cursor(m_text->document());
    cursor.movePosition(QTextCursor::End);
    const QString existing = m_text->toPlainText();
    if (!existing.isEmpty() && !existing.endsWith(QLatin1Char('\n'))) {
        const QChar last = existing.back();
        const QChar first = trimmed.front();
        const bool latinLast = last.unicode() < 128 && (last.isLetterOrNumber());
        const bool latinFirst = first.unicode() < 128 && (first.isLetterOrNumber());
        if (latinLast && latinFirst)
            cursor.insertText(QStringLiteral(" "));
    }
    cursor.insertText(trimmed);
    m_text->setTextCursor(cursor);
    m_text->ensureCursorVisible();
}

QString MainWindow::textForSpeech() const
{
    const QTextCursor cursor = m_text->textCursor();
    if (cursor.hasSelection()) {
        QString text = cursor.selectedText();
        text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
        return text;
    }
    return m_text->toPlainText();
}

void MainWindow::startRecording()
{
    if (m_recorder.isRunning())
        return;
    if (m_importMode) {
        setStatus(QStringLiteral("正在识别音频文件，请稍后再录音"));
        QSignalBlocker blocker(m_recordAction);
        m_recordAction->setChecked(false);
        return;
    }
    if (!ensureApiKey(QStringLiteral("开始录音"))) {
        QSignalBlocker blocker(m_recordAction);
        m_recordAction->setChecked(false);
        return;
    }
    if (m_player.state() != Player::State::Idle)
        m_player.stop();
    m_demoTimer->stop();

    m_asr.configure(m_settings.endpoint(), m_settings.effectiveApiKey());
    m_asrError.clear();
    m_dictationPcm.clear();
    m_sessionRecordingId.clear();
    m_sessionTranscript.clear();

    const QAudioDevice device = inputDevice();
    m_monitor->setSampleRate(16000);
    m_monitor->setMode(AudioMonitor::Mode::Bars);
    m_monitor->clear();
    m_monitor->setCaption(QStringLiteral("输入 · %1 · 16 kHz").arg(audio::displayName(device)));

    VadConfig config;
    config.silenceMs = m_settings.silenceMs;
    config.minSpeechMs = m_settings.minSpeechMs;
    config.maxSegmentMs = m_settings.maxSegmentMs;

    QString error;
    if (!m_recorder.start(device, config, &error)) {
        QMessageBox::warning(this, QStringLiteral("无法开始录音"), error);
        QSignalBlocker blocker(m_recordAction);
        m_recordAction->setChecked(false);
        setStatus(QStringLiteral("录音启动失败"));
        return;
    }

    m_stopAction->setEnabled(true);
    m_speakAction->setEnabled(false);
    updatePlayAction();
    setStatus(QStringLiteral("聆听中… 停顿一下就会断句"));
}

void MainWindow::stopRecording()
{
    if (!m_recorder.isRunning()) {
        QSignalBlocker blocker(m_recordAction);
        m_recordAction->setChecked(false);
        return;
    }
    m_recorder.stop();
    {
        QSignalBlocker blocker(m_recordAction);
        m_recordAction->setChecked(false);
    }
    saveDictationRecording();
    m_speakAction->setEnabled(true);
    updatePlayAction();
    m_stopAction->setEnabled(false);
    if (!m_sessionRecordingId.isEmpty())
        setStatus(QStringLiteral("结束录音 · 已保存到录音库"));
    else if (m_asr.busy())
        setStatus(QStringLiteral("结束录音 · 正在识别剩余段落…"));
    else
        setStatus(QStringLiteral("已停止"));
}

void MainWindow::saveDictationRecording()
{
    if (m_dictationPcm.size() < 16000)
        return;

    const qint64 durationMs = m_dictationPcm.size() / 32;
    const QByteArray wav = wavFromPcm(m_dictationPcm, 16000, 1);
    RecordingInfo info;
    info.title = m_store->nextTitle();
    info.createdAt = QDateTime::currentDateTime();
    info.durationMs = durationMs;
    info.sampleRate = 16000;
    info.channels = 1;
    info.bits = 16;
    info.transcript = m_sessionTranscript.trimmed();
    info.peaks = pcm::peaks(m_dictationPcm, 1, 180);

    QString error;
    const QString id = m_store->addWav(wav, info, &error);
    if (!id.isEmpty()) {
        m_sessionRecordingId = id;
    } else {
        m_asrError = QStringLiteral("录音保存失败：%1").arg(error);
    }
    m_dictationPcm.clear();
}

void MainWindow::stopEverything()
{
    if (m_recorder.isRunning())
        stopRecording();
    if (m_player.state() != Player::State::Idle)
        m_player.stop();
    if (m_demoTimer->isActive()) {
        m_demoTimer->stop();
        setStatus(QStringLiteral("演示已停止"));
    } else if (m_asr.busy()) {
        setStatus(QStringLiteral("已停止 · 正在识别剩余段落…"));
    } else {
        setStatus(QStringLiteral("已停止"));
    }
    m_stopAction->setEnabled(m_recorder.isRunning());
}

void MainWindow::speakText()
{
    if (m_importMode) {
        setStatus(QStringLiteral("正在识别音频文件，请稍后再朗读"));
        return;
    }
    if (m_recorder.isRunning())
        stopRecording();

    const QString text = textForSpeech().trimmed();
    if (text.isEmpty()) {
        setStatus(QStringLiteral("没有可朗读的文本"));
        return;
    }
    if (!ensureApiKey(QStringLiteral("朗读文本")))
        return;

    m_demoTimer->stop();
    const QAudioDevice device = outputDevice();
    m_monitor->setSampleRate(24000);
    m_monitor->setMode(AudioMonitor::Mode::Spectrogram);
    m_monitor->clear();
    m_monitor->setCaption(QStringLiteral("输出 · %1 · 24 kHz · 声谱图").arg(audio::displayName(device)));

    tts::Request request;
    request.baseUrl = m_settings.endpoint();
    request.apiKey = m_settings.effectiveApiKey();
    request.voiceId = m_settings.voiceId;
    request.filterMarkdown = m_settings.filterMarkdown;

    m_player.setOutputDevice(device);
    m_player.setVolume(m_volume->value() / 100.0);
    m_ttsChars = tts::billedChars(text);
    updateCost();
    m_player.play(text, request);
}

void MainWindow::togglePlayback()
{
    switch (m_player.state()) {
    case Player::State::Playing:
    case Player::State::Paused:
        m_player.togglePause();
        break;
    case Player::State::Finished:
        m_player.seek(0);
        break;
    case Player::State::Idle:
    case Player::State::Failed:
        speakText();
        break;
    case Player::State::Buffering:
        break;
    }
}

void MainWindow::exportText()
{
    const QString text = m_text->toPlainText();
    if (text.trimmed().isEmpty()) {
        setStatus(QStringLiteral("没有可导出的文本"));
        return;
    }
    const QString suggested = QDir::homePath() + QStringLiteral("/dvoice.txt");
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出文本"), suggested, QStringLiteral("文本文件 (*.txt)"));
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), file.errorString());
        return;
    }
    file.write(text.toUtf8());
    setStatus(QStringLiteral("已导出到 %1").arg(path));
}

void MainWindow::applySettings(const Settings &settings)
{
    const QString oldInput = m_settings.inputDevice;
    const QString oldOutput = m_settings.outputDevice;
    m_settings = settings;
    m_settings.save();

    m_asr.configure(m_settings.endpoint(), m_settings.effectiveApiKey());
    m_player.setOutputDevice(outputDevice());
    m_player.setVolume(m_settings.volume / 100.0);
    {
        QSignalBlocker blocker(m_volume);
        m_volume->setValue(m_settings.volume);
    }

    if (m_recorder.isRunning() && oldInput != m_settings.inputDevice) {
        stopRecording();
        setStatus(QStringLiteral("输入设备已更改，请重新开始录音"));
        return;
    }
    if (m_player.state() != Player::State::Idle && oldOutput != m_settings.outputDevice) {
        m_player.stop();
        setStatus(QStringLiteral("输出设备已更改，请重新朗读"));
        return;
    }
    if (m_library)
        m_library->applySettings(m_settings);
    setStatus(QStringLiteral("首选项已保存"));
}

PreferencesDialog *MainWindow::openPreferences()
{
    if (m_prefs) {
        m_prefs->raise();
        m_prefs->activateWindow();
        return m_prefs;
    }
    Settings latest = Settings::load();
    const QString sessionKey = m_settings.apiKey;
    const bool rememberKey = m_settings.rememberApiKey;
    if (!rememberKey && !sessionKey.isEmpty()) {
        latest.apiKey = sessionKey;
        latest.rememberApiKey = false;
    }
    latest.volume = m_volume->value();
    m_settings = latest;
    auto *dialog = new PreferencesDialog(m_settings, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_prefs = dialog;
    connect(dialog, &QDialog::accepted, this, [this, dialog] {
        applySettings(dialog->settings());
    });
    dialog->show();
    dialog->raise();
    return dialog;
}

void MainWindow::openMicTest()
{
    auto *dialog = new MicTestDialog(m_settings, this);
    dialog->show();
    dialog->raise();
}

void MainWindow::openSpeakerTest()
{
    auto *dialog = new SpeakerTestDialog(m_settings, this);
    dialog->show();
    dialog->raise();
}

void MainWindow::openAbout()
{
    showAboutDialog(this);
}

void MainWindow::showLibraryTab()
{
    m_tabs->setCurrentIndex(1);
}

void MainWindow::onTabChanged(int index)
{
    const bool library = index == 1;
    if (library) {
        if (m_recorder.isRunning())
            stopRecording();
        if (m_player.state() != Player::State::Idle)
            m_player.stop();
        m_toolbar->setVisible(false);
        statusBar()->setVisible(false);
        m_recordAction->setEnabled(false);
        m_stopAction->setEnabled(false);
        m_speakAction->setEnabled(false);
        m_playAction->setEnabled(false);
        m_library->applySettings(m_settings);
        m_library->refresh();
    } else {
        m_library->stopPlayback();
        m_toolbar->setVisible(true);
        statusBar()->setVisible(true);
        m_recordAction->setEnabled(!m_importMode);
        m_speakAction->setEnabled(!m_importMode && !m_recorder.isRunning());
        updatePlayAction();
        m_stopAction->setEnabled(m_recorder.isRunning()
                                 || m_player.state() == Player::State::Playing
                                 || m_player.state() == Player::State::Paused
                                 || m_player.state() == Player::State::Buffering);
    }
}

void MainWindow::openAudioFile(const QString &path)
{
    if (path.isEmpty())
        return;
    if (m_importMode || m_asr.busy()) {
        QMessageBox::information(this, QStringLiteral("请稍等"),
                                 QStringLiteral("当前还有识别任务没有完成，请稍后再打开音频。"));
        return;
    }
    if (m_recorder.isRunning())
        stopRecording();
    if (m_player.state() != Player::State::Idle)
        m_player.stop();
    if (!ensureApiKey(QStringLiteral("识别音频文件")))
        return;

    m_tabs->setCurrentIndex(0);
    setStatus(QStringLiteral("正在解码 %1…").arg(QFileInfo(path).fileName()));
    QApplication::processEvents();

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const audiofile::DecodedAudio decoded = audiofile::decodeToMono16(path);
    QApplication::restoreOverrideCursor();
    if (!decoded.ok()) {
        setStatus(QStringLiteral("解码失败"));
        QMessageBox::warning(this, QStringLiteral("无法打开音频"), decoded.error);
        return;
    }

    const QString name = QFileInfo(path).fileName();
    const int feedBytes = qMin(decoded.pcm16.size(), 16000 * 2 * 10);
    m_monitor->setSampleRate(16000);
    m_monitor->setMode(AudioMonitor::Mode::Spectrogram);
    m_monitor->clear();
    m_monitor->feed(decoded.pcm16.left(feedBytes));
    m_monitor->setCaption(QStringLiteral("文件 · %1 · 16 kHz%2")
                              .arg(name, decoded.durationMs > 10000
                                             ? QStringLiteral(" · 前 10 秒")
                                             : QString()));

    m_asr.configure(m_settings.endpoint(), m_settings.effectiveApiKey());
    m_asrError.clear();
    m_importMode = true;
    m_importTotal = 0;
    m_importDone = 0;
    m_importName = name;
    m_recordAction->setEnabled(false);
    m_speakAction->setEnabled(false);
    m_playAction->setEnabled(false);
    m_stopAction->setEnabled(false);

    Recorder segmenter;
    connect(&segmenter, &Recorder::segment, this,
            [this](const QByteArray &pcm, int startMs, int endMs) {
                ++m_importTotal;
                m_asr.enqueue(pcm, 16000, startMs, endMs);
            });

    VadConfig config;
    config.silenceMs = m_settings.silenceMs;
    config.minSpeechMs = m_settings.minSpeechMs;
    config.maxSegmentMs = m_settings.maxSegmentMs;
    segmenter.beginTestFeed(config);
    segmenter.feedTestPcm(decoded.pcm16);
    segmenter.endTestFeed();

    if (m_importTotal == 0) {
        m_importMode = false;
        m_recordAction->setEnabled(true);
        m_speakAction->setEnabled(true);
        updatePlayAction();
        setStatus(QStringLiteral("没有检测到语音 · %1").arg(name));
        return;
    }

    setStatus(QStringLiteral("识别 %1 · 共 %2 段…").arg(name).arg(m_importTotal));
}

void MainWindow::finishImport()
{
    m_importMode = false;
    m_recordAction->setEnabled(true);
    m_speakAction->setEnabled(true);
    m_stopAction->setEnabled(false);
    updatePlayAction();
    if (!m_asrError.isEmpty()) {
        setStatus(m_asrError);
        return;
    }
    setStatus(QStringLiteral("识别完成 · %1 · %2 段").arg(m_importName).arg(m_importTotal));
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    const auto audioPath = [](const QMimeData *mime) {
        if (!mime || !mime->hasUrls())
            return QString();
        for (const QUrl &url : mime->urls()) {
            if (!url.isLocalFile())
                continue;
            const QString path = url.toLocalFile();
            if (audiofile::isAudioFile(path))
                return path;
        }
        return QString();
    };

    if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
        auto *drag = static_cast<QDropEvent *>(event);
        if (!audioPath(drag->mimeData()).isEmpty()) {
            drag->acceptProposedAction();
            setStatus(QStringLiteral("松开以识别音频文件"));
            return true;
        }
    } else if (event->type() == QEvent::Drop) {
        auto *drop = static_cast<QDropEvent *>(event);
        const QString path = audioPath(drop->mimeData());
        if (!path.isEmpty()) {
            drop->acceptProposedAction();
            openAudioFile(path);
            return true;
        }
    } else if (event->type() == QEvent::DragLeave) {
        setStatus(QStringLiteral("就绪"));
        return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::showDemo()
{
    m_text->setPlainText(QStringLiteral(
        "今天下午把 dvoice 的录音链路跑通了。\n\n"
        "识别接口本身不是流式的，所以这里靠停顿断句：每说完一句，就把刚结束的这一小段上传，"
        "识别结果按顺序接在后面，边说边出字。\n\n"
        "播放这边是真流式：合成的同时就在出声，等整段下载完，进度条就可以拖动定位。"));

    m_monitor->setSampleRate(24000);
    m_monitor->setMode(AudioMonitor::Mode::Spectrogram);
    m_monitor->clear();
    m_monitor->setCaption(QStringLiteral("输出 · 演示数据 · 24 kHz · 声谱图"));
    m_progress->setRange(0, 31000);
    m_progress->setValue(12000);
    m_progress->setEnabled(false);
    m_time->setText(QStringLiteral("00:12 / 00:31"));
    m_recognizedMs = 42000;
    m_ttsChars = 168;
    updateCost();
    setStatus(QStringLiteral("演示模式 · 未连接接口"));

    if (!m_demoTimer)
        return;
    m_demoPhase = 0.0;
    m_monitor->feed(demoAudio(m_demoPhase, 24000 * 8));
    m_demoTimer->start();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_recorder.isRunning())
        stopRecording();
    else
        m_recorder.stop();
    m_player.stop();
    if (m_demoTimer)
        m_demoTimer->stop();
    m_settings.volume = m_volume->value();
    m_settings.save();
    QSettings store;
    store.setValue(QStringLiteral("window/geometry"), saveGeometry());
    store.setValue(QStringLiteral("window/layout"), 2);
    QMainWindow::closeEvent(event);
}

void MainWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange && !m_applyingStyle) {
        m_applyingStyle = true;
        qApp->setStyleSheet(theme::styleSheet(qApp->palette()));
        refreshIcons();
        m_applyingStyle = false;
    }
    QMainWindow::changeEvent(event);
}
