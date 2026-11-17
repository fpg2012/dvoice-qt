#include "theme.h"

#include <QApplication>
#include <QFile>
#include <QPainter>
#include <QPalette>
#include <QSvgRenderer>

namespace theme {

bool isDark()
{
    return QApplication::palette().color(QPalette::Window).lightness() < 128;
}

QColor tone(const QColor &color)
{
    return isDark() ? color.lighter(130) : color.darker(112);
}

QIcon icon(const QString &name)
{
    QFile file(QStringLiteral(":/icons/%1.svg").arg(name));
    if (!file.open(QIODevice::ReadOnly))
        return {};

    QString svg = QString::fromUtf8(file.readAll());
    const QColor ink = QApplication::palette().color(QPalette::WindowText);
    svg.replace(QLatin1String("currentColor"), ink.name(QColor::HexRgb));

    QSvgRenderer renderer(svg.toUtf8());
    if (!renderer.isValid())
        return {};

    QIcon result;
    for (const int dpr : {1, 2, 3}) {
        const int pixels = 22 * dpr;
        QPixmap pixmap(pixels, pixels);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        renderer.render(&painter, QRectF(0, 0, pixels, pixels));
        painter.end();
        pixmap.setDevicePixelRatio(dpr);
        result.addPixmap(pixmap);
    }
    return result;
}

QIcon appIcon()
{
    return QIcon(QStringLiteral(":/icons/app.svg"));
}

void applyScheme(bool dark)
{
    QPalette palette;
    const QColor window = dark ? QColor(0x1e, 0x1e, 0x1e) : QColor(0xf4, 0xf4, 0xf4);
    const QColor base = dark ? QColor(0x14, 0x14, 0x14) : QColor(0xff, 0xff, 0xff);
    const QColor button = dark ? QColor(0x2a, 0x2a, 0x2a) : QColor(0xf7, 0xf7, 0xf7);
    const QColor text = dark ? QColor(0xe8, 0xe8, 0xe8) : QColor(0x17, 0x17, 0x17);
    const QColor accent = dark ? QColor(0x46, 0x96, 0x6a) : QColor(0x2f, 0x7d, 0x4f);
    const QColor mid = dark ? QColor(0x3a, 0x3a, 0x3a) : QColor(0xc9, 0xc9, 0xc9);

    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, button);
    palette.setColor(QPalette::ToolTipBase, button);
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, button);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::BrightText, Qt::white);
    palette.setColor(QPalette::Link, accent);
    palette.setColor(QPalette::Highlight, accent);
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::PlaceholderText, dark ? QColor(0x8a, 0x8a, 0x8a)
                                                     : QColor(0x7a, 0x7a, 0x7a));
    palette.setColor(QPalette::Mid, mid);
    palette.setColor(QPalette::Midlight, mid.lighter(120));
    palette.setColor(QPalette::Dark, dark ? QColor(0x10, 0x10, 0x10) : QColor(0x9a, 0x9a, 0x9a));
    palette.setColor(QPalette::Light, dark ? QColor(0x50, 0x50, 0x50) : Qt::white);
    palette.setColor(QPalette::Shadow, Qt::black);

    const QColor disabled = dark ? QColor(0x77, 0x77, 0x77) : QColor(0x9a, 0x9a, 0x9a);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    palette.setColor(QPalette::Disabled, QPalette::Text, disabled);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    palette.setColor(QPalette::Disabled, QPalette::Highlight, mid);
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, disabled);

    QApplication::setPalette(palette);
}

QString styleSheet(const QPalette &palette)
{
    const QColor separator = palette.color(QPalette::Mid);
    const QColor accent = palette.color(QPalette::Highlight);
    const QColor text = palette.color(QPalette::WindowText);
    const QColor disabled = palette.color(QPalette::Disabled, QPalette::WindowText);

    auto rgba = [](const QColor &color, int alpha) {
        return QStringLiteral("rgba(%1,%2,%3,%4)")
            .arg(color.red())
            .arg(color.green())
            .arg(color.blue())
            .arg(alpha);
    };

    return QStringLiteral(R"(
QToolBar { border: none; padding: 6px 10px; spacing: 3px; }
QToolBar::separator { background: %1; width: 1px; margin: 6px 8px; }
QToolButton { border: none; border-radius: 7px; padding: 6px; }
QToolButton:hover { background: %2; }
QToolButton:pressed { background: %3; }
QToolButton:checked { background: %3; }
QToolButton:disabled { color: %7; }
QMenuBar::item { padding: 4px 9px; border-radius: 5px; }
QMenuBar::item:selected { background: %2; }
QStatusBar { border-top: 1px solid %1; }
QStatusBar::item { border: none; }
QPlainTextEdit {
    border: 1px solid %1;
    border-radius: 8px;
    padding: 12px;
    selection-background-color: %6;
    selection-color: %4;
}
QComboBox, QLineEdit, QSpinBox, QDoubleSpinBox {
    padding: 4px 8px;
    border: 1px solid %1;
    border-radius: 6px;
    min-height: 22px;
}
QComboBox:focus, QLineEdit:focus, QSpinBox:focus { border-color: %6; }
QComboBox::drop-down { border: none; width: 20px; }
QTabWidget::pane { border: 1px solid %1; border-radius: 8px; top: -1px; }
QTabBar::tab { padding: 6px 14px; border-radius: 6px; margin: 0 2px 4px 2px; }
QTabBar::tab:selected { background: %3; }
QTabBar::tab:hover { background: %2; }
QSlider::groove:horizontal { height: 4px; background: %5; border-radius: 2px; }
QSlider::sub-page:horizontal { background: %6; border-radius: 2px; }
QSlider::handle:horizontal {
    width: 14px; height: 14px; margin: -5px 0;
    border-radius: 7px; background: %4; border: 1px solid %1;
}
QSlider::handle:horizontal:hover { border-color: %6; }
QSlider::handle:horizontal:disabled { background: %5; border-color: %1; }
QSlider::sub-page:horizontal:disabled { background: %5; }
QSlider::groove:horizontal:disabled { background: %5; }
QProgressBar { border: 1px solid %1; border-radius: 4px; height: 8px; text-align: center; }
QProgressBar::chunk { background: %6; border-radius: 3px; }
QCheckBox::indicator {
    width: 15px; height: 15px;
    border: 1px solid %1; border-radius: 3px; background: %8;
}
QCheckBox::indicator:hover { border-color: %9; }
QCheckBox::indicator:checked { background: %9; border-color: %9; image: url(:/icons/check.svg); }
QCheckBox::indicator:disabled { border-color: %1; }
QPushButton { padding: 5px 16px; border: 1px solid %1; border-radius: 6px; }
QPushButton:hover { background: %2; }
QPushButton:pressed { background: %3; }
QPushButton:default { border-color: %6; }
QGroupBox { border: 1px solid %1; border-radius: 8px; margin-top: 12px; padding-top: 10px; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }
)")
        .arg(separator.name(),
             rgba(accent, 28),
             rgba(accent, 56),
             text.name(),
             rgba(accent, 70),
             rgba(accent, 190),
             disabled.name(),
             palette.color(QPalette::Base).name(),
             accent.name());
}

} // namespace theme
