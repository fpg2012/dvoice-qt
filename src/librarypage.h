// 录音 & 回放页：录音库列表 + 播放控制。
#pragma once

#include "core/recordingstore.h"
#include "core/settings.h"

#include <QHash>
#include <QWidget>

class QAudioOutput;
class QFileSystemWatcher;
class QLabel;
class QMediaPlayer;
class QScrollArea;
class QTimer;
class QVBoxLayout;
class RecordingCard;

class LibraryPage : public QWidget
{
    Q_OBJECT

public:
    explicit LibraryPage(RecordingStore *store, const Settings &settings,
                         QWidget *parent = nullptr);
    ~LibraryPage() override;

    void refresh();
    void applySettings(const Settings &settings);
    void stopPlayback();
    void playFirstForTest();
    void openFirstDetailForTest();
    void startRecording();
    void importAudio();

private:
    void rebuild();
    void togglePlayback(const Recording &recording);
    void startPlayback(const Recording &recording, qint64 positionMs);
    void skip(const Recording &recording, int deltaMs);
    void seek(const Recording &recording, qreal ratio);
    void renameRecording(const Recording &recording);
    void deleteRecording(const Recording &recording);
    void selectRecording(const QString &id);
    void openDetail(const Recording &recording);
    void exportRecording(const Recording &recording);
    void updateCardStates();
    void setStatus(const QString &text);

    RecordingStore *m_store;
    Settings m_settings;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audio = nullptr;
    QScrollArea *m_scroll = nullptr;
    QVBoxLayout *m_listLayout = nullptr;
    QLabel *m_empty = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_count = nullptr;
    QFileSystemWatcher *m_watcher = nullptr;
    QTimer *m_refreshTimer = nullptr;
    QHash<QString, RecordingCard *> m_cards;
    QVector<Recording> m_recordings;
    QString m_currentId;
    QString m_selectedId;
    qint64 m_pendingSeekMs = -1;
};
