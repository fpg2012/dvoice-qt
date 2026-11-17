// 首选项与关于对话框。
#pragma once

#include "core/settings.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;
class QLabel;
class QSlider;
class QTabWidget;

class PreferencesDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PreferencesDialog(const Settings &settings, QWidget *parent = nullptr);

    Settings settings() const { return m_result; }
    void selectTab(int index);

private:
    void buildUi();
    void load(const Settings &settings);
    void accept() override;

    Settings m_result;
    QLineEdit *m_key = nullptr;
    QLineEdit *m_baseUrl = nullptr;
    QCheckBox *m_showKey = nullptr;
    QCheckBox *m_remember = nullptr;
    QComboBox *m_input = nullptr;
    QComboBox *m_output = nullptr;
    QComboBox *m_voice = nullptr;
    QCheckBox *m_filterMarkdown = nullptr;
    QSlider *m_silence = nullptr;
    QLabel *m_silenceValue = nullptr;
    QSlider *m_minSpeech = nullptr;
    QLabel *m_minSpeechValue = nullptr;
    QSlider *m_maxSegment = nullptr;
    QLabel *m_maxSegmentValue = nullptr;
    QPushButton *m_refresh = nullptr;
    QTabWidget *m_tabs = nullptr;
};

void showAboutDialog(QWidget *parent);
