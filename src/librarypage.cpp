#include "librarypage.h"

#include "core/audiofile.h"
#include "core/audiodevices.h"
#include "core/pcm.h"
#include "recorddialog.h"
#include "recordingcard.h"
#include "theme.h"

#include <QApplication>
#include <QDateTime>
#include <QAudioOutput>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QLabel>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

LibraryPage::LibraryPage(RecordingStore *store, const Settings &settings, QWidget *parent)
    : QWidget(parent)
    , m_store(store)
    , m_settings(settings)
    , m_player(new QMediaPlayer(this))
    , m_audio(new QAudioOutput(this))
{
    m_player->setAudioOutput(m_audio);
    m_audio->setVolume(m_settings.volume / 100.0);
    m_audio->setDevice(audio::findOutput(m_settings.outputDevice));

    auto *recordButton = new QPushButton(theme::icon(QStringLiteral("record")),
                                        QStringLiteral("录音"));
    recordButton->setFixedHeight(34);
    recordButton->setMinimumWidth(108);
    recordButton->setIconSize(QSize(16, 16));

    auto *importButton = new QPushButton(theme::icon(QStringLiteral("import")),
                                         QStringLiteral("导入音频"));
    importButton->setFixedHeight(34);
    importButton->setMinimumWidth(108);
    importButton->setIconSize(QSize(16, 16));
    auto *folderButton = new QPushButton(theme::icon(QStringLiteral("folder")), QString());
    folderButton->setToolTip(QStringLiteral("打开录音库文件夹"));
    folderButton->setFixedSize(40, 34);
    folderButton->setIconSize(QSize(16, 16));

    m_count = new QLabel;
    m_count->setForegroundRole(QPalette::PlaceholderText);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(8);
    header->addWidget(recordButton);
    header->addWidget(importButton);
    header->addWidget(folderButton);
    header->addStretch(1);
    header->addWidget(m_count);

    m_scroll = new QScrollArea;
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *container = new QWidget;
    m_listLayout = new QVBoxLayout(container);
    m_listLayout->setContentsMargins(0, 0, 6, 0);
    m_listLayout->setSpacing(12);
    m_scroll->setWidget(container);

    m_empty = new QLabel(QStringLiteral("还没有录音。点「录音」开始，或把音频文件拖进来。"));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setForegroundRole(QPalette::PlaceholderText);
    m_empty->setMinimumHeight(120);

    m_status = new QLabel;
    m_status->setForegroundRole(QPalette::PlaceholderText);
    m_status->setWordWrap(true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 16, 20, 12);
    layout->setSpacing(12);
    layout->addLayout(header);
    layout->addWidget(m_empty);
    layout->addWidget(m_scroll, 1);
    layout->addWidget(m_status);

    connect(recordButton, &QPushButton::clicked, this, &LibraryPage::startRecording);
    connect(importButton, &QPushButton::clicked, this, &LibraryPage::importAudio);
    connect(folderButton, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_store->directory()));
    });

    connect(m_store, &RecordingStore::changed, this, [this] {
        QTimer::singleShot(0, this, &LibraryPage::refresh);
    });

    QString error;
    m_store->ensureDirectory(&error);
    m_watcher = new QFileSystemWatcher(this);
    if (QDir(m_store->directory()).exists())
        m_watcher->addPath(m_store->directory());
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(300);
    connect(m_refreshTimer, &QTimer::timeout, this, &LibraryPage::refresh);
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] {
        m_refreshTimer->start();
    });

    connect(m_player, &QMediaPlayer::positionChanged, this, [this](qint64 position) {
        if (m_currentId.isEmpty())
            return;
        if (RecordingCard *card = m_cards.value(m_currentId)) {
            const qint64 duration = m_player->duration();
            if (duration > 0)
                card->setProgress(double(position) / duration);
        }
    });
    connect(m_player, &QMediaPlayer::durationChanged, this, [this](qint64 duration) {
        if (m_pendingSeekMs >= 0) {
            m_player->setPosition(qBound<qint64>(0, m_pendingSeekMs, duration));
            m_pendingSeekMs = -1;
        }
    });
    connect(m_player, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
        updateCardStates();
        if (state == QMediaPlayer::PlayingState)
            setStatus(QStringLiteral("正在播放…"));
        else if (state == QMediaPlayer::PausedState)
            setStatus(QStringLiteral("已暂停"));
    });
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia) {
            if (RecordingCard *card = m_cards.value(m_currentId))
                card->setProgress(1.0);
            updateCardStates();
            setStatus(QStringLiteral("播放完成"));
        }
    });
    connect(m_player, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error, const QString &message) {
                setStatus(QStringLiteral("播放失败：%1").arg(message));
            });

    refresh();
}

LibraryPage::~LibraryPage()
{
    m_player->stop();
}

void LibraryPage::refresh()
{
    if (!QDir(m_store->directory()).exists()) {
        m_store->ensureDirectory();
        if (QDir(m_store->directory()).exists()
            && !m_watcher->directories().contains(m_store->directory())) {
            m_watcher->addPath(m_store->directory());
        }
    }
    rebuild();
}

void LibraryPage::rebuild()
{
    stopPlayback();
    m_cards.clear();

    while (QLayoutItem *item = m_listLayout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }

    m_recordings = m_store->list();
    m_empty->setVisible(m_recordings.isEmpty());
    m_count->setText(m_recordings.isEmpty()
                         ? QString()
                         : QStringLiteral("%1 条录音").arg(m_recordings.size()));

    for (const Recording &recording : m_recordings) {
        auto *card = new RecordingCard(recording);
        m_cards.insert(recording.id, card);
        connect(card, &RecordingCard::playRequested, this, [this, recording] {
            togglePlayback(recording);
        });
        connect(card, &RecordingCard::skipRequested, this, [this, recording](int delta) {
            skip(recording, delta);
        });
        connect(card, &RecordingCard::seekRequested, this, [this, recording](qreal ratio) {
            seek(recording, ratio);
        });
        connect(card, &RecordingCard::renameRequested, this, [this, recording] {
            renameRecording(recording);
        });
        connect(card, &RecordingCard::deleteRequested, this, [this, recording] {
            deleteRecording(recording);
        });
        connect(card, &RecordingCard::exportRequested, this, [this, recording] {
            exportRecording(recording);
        });
        m_listLayout->addWidget(card);
    }
    m_listLayout->addStretch(1);
    m_cards.remove(m_currentId);
}

void LibraryPage::togglePlayback(const Recording &recording)
{
    if (m_currentId == recording.id) {
        if (m_player->playbackState() == QMediaPlayer::PlayingState)
            m_player->pause();
        else
            m_player->play();
        updateCardStates();
        return;
    }

    startPlayback(recording, 0);
}

void LibraryPage::startPlayback(const Recording &recording, qint64 positionMs)
{
    m_player->stop();
    m_currentId = recording.id;
    m_pendingSeekMs = positionMs > 0 ? positionMs : -1;
    m_audio->setDevice(audio::findOutput(m_settings.outputDevice));
    m_player->setSource(QUrl::fromLocalFile(recording.path));
    m_player->play();

    for (auto it = m_cards.begin(); it != m_cards.end(); ++it)
        it.value()->setSelected(it.key() == m_currentId);
    if (RecordingCard *card = m_cards.value(m_currentId))
        card->setProgress(0.0);
    updateCardStates();
    setStatus(QStringLiteral("正在播放 · %1").arg(recording.title));
}

void LibraryPage::skip(const Recording &recording, int deltaMs)
{
    if (m_currentId != recording.id) {
        startPlayback(recording, qMax<qint64>(0, deltaMs));
        return;
    }
    const qint64 duration = m_player->duration() > 0 ? m_player->duration() : recording.durationMs;
    m_player->setPosition(qBound<qint64>(0, m_player->position() + deltaMs, duration));
}

void LibraryPage::seek(const Recording &recording, qreal ratio)
{
    const qint64 target = qint64(qBound(0.0, ratio, 1.0) * recording.durationMs);
    if (m_currentId != recording.id) {
        startPlayback(recording, target);
        return;
    }
    const qint64 duration = m_player->duration() > 0 ? m_player->duration() : recording.durationMs;
    m_player->setPosition(qBound<qint64>(0, target, duration));
}

void LibraryPage::renameRecording(const Recording &recording)
{
    bool ok = false;
    const QString title = QInputDialog::getText(
        this, QStringLiteral("重命名录音"), QStringLiteral("标题"),
        QLineEdit::Normal, recording.title, &ok);
    if (!ok || title.trimmed().isEmpty())
        return;
    Recording updated = recording;
    updated.title = title.trimmed();
    QString error;
    if (!m_store->update(updated, &error))
        QMessageBox::warning(this, QStringLiteral("重命名失败"), error);
}

void LibraryPage::deleteRecording(const Recording &recording)
{
    if (QMessageBox::question(this, QStringLiteral("删除录音"),
                              QStringLiteral("确定删除「%1」？").arg(recording.title))
        != QMessageBox::Yes) {
        return;
    }
    if (m_currentId == recording.id)
        stopPlayback();
    QString error;
    if (!m_store->remove(recording, &error))
        QMessageBox::warning(this, QStringLiteral("删除失败"), error);
}

void LibraryPage::exportRecording(const Recording &recording)
{
    const QFileInfo source(recording.path);
    const QString suffix = source.suffix().isEmpty() ? QStringLiteral("wav")
                                                      : source.suffix();
    QString base = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (base.isEmpty())
        base = QDir::homePath();
    const QString suggested = QDir(base).filePath(
        QStringLiteral("%1.%2").arg(recording.title, suffix));
    const QString target = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出音频"), suggested,
        QStringLiteral("%1 文件 (*.%1)").arg(suffix));
    if (target.isEmpty())
        return;
    if (QFileInfo(target).absoluteFilePath() == source.absoluteFilePath())
        return;
    QFile::remove(target);
    if (!QFile::copy(source.absoluteFilePath(), target)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"),
                             QStringLiteral("无法写入 %1").arg(target));
        return;
    }
    setStatus(QStringLiteral("已导出到 %1").arg(target));
}

void LibraryPage::updateCardStates()
{
    for (auto it = m_cards.begin(); it != m_cards.end(); ++it) {
        const bool current = it.key() == m_currentId;
        it.value()->setPlaying(current && m_player->playbackState() == QMediaPlayer::PlayingState);
        it.value()->setSelected(current);
    }
}

void LibraryPage::stopPlayback()
{
    if (m_player->playbackState() != QMediaPlayer::StoppedState)
        m_player->stop();
    if (RecordingCard *card = m_cards.value(m_currentId)) {
        card->setPlaying(false);
        card->setSelected(false);
        card->setProgress(0.0);
    }
    m_currentId.clear();
    m_pendingSeekMs = -1;
}

void LibraryPage::applySettings(const Settings &settings)
{
    const bool outputChanged = settings.outputDevice != m_settings.outputDevice;
    m_settings = settings;
    m_audio->setVolume(m_settings.volume / 100.0);
    if (outputChanged) {
        stopPlayback();
        m_audio->setDevice(audio::findOutput(m_settings.outputDevice));
    }
}

void LibraryPage::startRecording()
{
    m_settings = Settings::load();
    auto *dialog = new RecordDialog(m_store, m_settings, this);
    connect(dialog, &RecordDialog::saved, this, [this](const QString &id) {
        refresh();
        setStatus(QStringLiteral("已保存录音 %1").arg(id));
    });
    dialog->exec();
    refresh();
}

void LibraryPage::importAudio()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("导入音频"), QDir::homePath(), audiofile::nameFilters().join(
                                                              QStringLiteral(";;")));
    if (path.isEmpty())
        return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const audiofile::DecodedAudio decoded = audiofile::decodeToMono16(path);
    QApplication::restoreOverrideCursor();
    if (!decoded.ok()) {
        QMessageBox::warning(this, QStringLiteral("导入失败"), decoded.error);
        return;
    }

    RecordingInfo info;
    info.title = m_store->nextTitle();
    info.createdAt = QDateTime::currentDateTime();
    info.durationMs = decoded.durationMs;
    info.sampleRate = 16000;
    info.channels = 1;
    info.bits = 16;
    info.peaks = pcm::peaks(decoded.pcm16, 1, 180);

    QString error;
    const QString id = m_store->addFile(path, info, &error);
    if (id.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("导入失败"), error);
        return;
    }
    refresh();
    setStatus(QStringLiteral("已导入 %1").arg(QFileInfo(path).fileName()));
}

void LibraryPage::playFirstForTest()
{
    if (!m_recordings.isEmpty()) {
        m_audio->setMuted(true);
        togglePlayback(m_recordings.first());
    }
}

void LibraryPage::setStatus(const QString &text)
{
    m_status->setText(text);
}
