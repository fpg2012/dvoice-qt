// 录音库卡片：标题、时间、时长、静态波形、播放/跳转/重命名/删除。
#pragma once

#include "core/recordingstore.h"

#include <QFrame>

class QLabel;
class QToolButton;
class WaveformView;

class RecordingCard : public QFrame
{
    Q_OBJECT

public:
    explicit RecordingCard(const Recording &recording, QWidget *parent = nullptr);

    Recording recording() const { return m_recording; }
    void setPlaying(bool playing);
    void setProgress(qreal progress);
    void setSelected(bool selected);

signals:
    void playRequested();
    void skipRequested(int deltaMs);
    void seekRequested(qreal ratio);
    void renameRequested();
    void exportRequested();
    void deleteRequested();
    void clicked();
    void doubleClicked();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:

    Recording m_recording;
    QLabel *m_title = nullptr;
    QLabel *m_date = nullptr;
    QLabel *m_duration = nullptr;
    WaveformView *m_wave = nullptr;
    QToolButton *m_play = nullptr;
    QWidget *m_controls = nullptr;
    bool m_selected = false;
};
