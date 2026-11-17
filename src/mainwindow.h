// 主窗口：录制 & 转写 / 录制 & 回放两个标签页。
#pragma once

#include "core/asr.h"
#include "core/player.h"
#include "core/recorder.h"
#include "core/settings.h"

#include <QAudioDevice>
#include <QByteArray>
#include <QMainWindow>
#include <QPointer>

class QAction;
class QEvent;
class QLabel;
class QPlainTextEdit;
class QSlider;
class QTabWidget;
class QTimer;
class QToolBar;
class AudioMonitor;
class LibraryPage;
class PreferencesDialog;
class RecordingStore;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void showDemo();
    PreferencesDialog *openPreferences();
    void openMicTest();
    void openSpeakerTest();
    void openAbout();

    void openAudioFile(const QString &path);
    void showLibraryTab();

    // 调试用：从命令行驱动一次实时听写。
    void startDictationForTest() { startRecording(); }
    void stopDictationForTest() { stopRecording(); }

protected:
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildUi();
    void buildActions();
    void buildMenus();
    void connectSignals();

    void applySettings(const Settings &settings);
    void refreshIcons();
    void setStatus(const QString &text);
    void updateCost();
    void updatePlayAction();
    void updateTransport(qint64 position, qint64 duration, qint64 buffered);
    void onTabChanged(int index);

    void appendTranscript(const QString &text);
    bool ensureApiKey(const QString &action);
    QAudioDevice inputDevice() const;
    QAudioDevice outputDevice() const;
    QString textForSpeech() const;

    void startRecording();
    void stopRecording();
    void saveDictationRecording();
    void stopEverything();
    void speakText();
    void togglePlayback();
    void exportText();

    void startImport(const QString &path);
    void finishImport();

    Settings m_settings;
    RecordingStore *m_store = nullptr;
    Recorder m_recorder;
    asr::Queue m_asr;
    Player m_player;

    QTabWidget *m_tabs = nullptr;
    QWidget *m_transcribePage = nullptr;
    LibraryPage *m_library = nullptr;

    QPlainTextEdit *m_text = nullptr;
    AudioMonitor *m_monitor = nullptr;
    QToolBar *m_toolbar = nullptr;
    QSlider *m_progress = nullptr;
    QSlider *m_volume = nullptr;
    QLabel *m_time = nullptr;
    QLabel *m_state = nullptr;
    QLabel *m_usage = nullptr;
    QLabel *m_volumeIcon = nullptr;

    QAction *m_recordAction = nullptr;
    QAction *m_stopAction = nullptr;
    QAction *m_speakAction = nullptr;
    QAction *m_respeakAction = nullptr;
    QAction *m_playAction = nullptr;
    QAction *m_settingsAction = nullptr;

    QTimer *m_demoTimer = nullptr;
    QPointer<PreferencesDialog> m_prefs;

    qint64 m_recognizedMs = 0;
    int m_ttsChars = 0;
    QString m_asrError;
    double m_demoPhase = 0.0;
    bool m_applyingStyle = false;

    // 转写页录音保存到录音库。
    QByteArray m_dictationPcm;
    QString m_sessionRecordingId;
    QString m_sessionTranscript;

    // 文件识别进度。
    bool m_importMode = false;
    int m_importTotal = 0;
    int m_importDone = 0;
    QString m_importName;
};
