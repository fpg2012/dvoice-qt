// 主窗口：录音转写、文本朗读、播放控制与波形监视。
#pragma once

#include "core/asr.h"
#include "core/player.h"
#include "core/recorder.h"
#include "core/settings.h"

#include <QAudioDevice>
#include <QMainWindow>
#include <QPointer>

class QAction;
class QLabel;
class QPlainTextEdit;
class QSlider;
class QTimer;
class QToolBar;
class AudioMonitor;
class PreferencesDialog;

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

protected:
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;

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

    void appendTranscript(const QString &text);
    bool ensureApiKey(const QString &action);
    QAudioDevice inputDevice() const;
    QAudioDevice outputDevice() const;
    QString textForSpeech() const;

    void startRecording();
    void stopRecording();
    void stopEverything();
    void speakText();
    void togglePlayback();
    void exportText();

    Settings m_settings;
    Recorder m_recorder;
    asr::Queue m_asr;
    Player m_player;

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
    QAction *m_playAction = nullptr;
    QAction *m_settingsAction = nullptr;

    QTimer *m_demoTimer = nullptr;
    QPointer<PreferencesDialog> m_prefs;

    qint64 m_recognizedMs = 0;
    int m_ttsChars = 0;
    QString m_asrError;
    double m_demoPhase = 0.0;
    bool m_applyingStyle = false;
};
