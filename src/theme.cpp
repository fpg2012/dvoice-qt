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
    const QColor accent = palette.color(QPalette::Highlight);
    const QColor text = palette.color(QPalette::WindowText);

    auto rgba = [](const QColor &color, int alpha) {
        return QStringLiteral("rgba(%1,%2,%3,%4)")
            .arg(color.red())
            .arg(color.green())
            .arg(color.blue())
            .arg(alpha);
    };

    // 只定制播放相关控件；按钮、输入框、标签页全部使用 Qt 默认外观。
    return QStringLiteral(R"(
QToolBar { border: none; spacing: 2px; }
QMenuBar { background: palette(window); color: palette(window-text); }
QMenuBar::item { background: transparent; color: palette(window-text); padding: 4px 9px; }
QMenuBar::item:selected { background: palette(highlight); color: palette(highlighted-text); }
QSlider::groove:horizontal { height: 4px; background: %1; border-radius: 2px; }
QSlider::sub-page:horizontal { background: %2; border-radius: 2px; }
QSlider::handle:horizontal {
    width: 14px; height: 14px; margin: -5px 0;
    border-radius: 7px; background: %3; border: 1px solid %1;
}
QSlider::handle:horizontal:hover { border-color: %2; }
QSlider::handle:horizontal:disabled { background: %1; border-color: %1; }
QSlider::sub-page:horizontal:disabled { background: %1; }
QSlider::groove:horizontal:disabled { background: %1; }
QProgressBar { border: 1px solid %1; border-radius: 4px; height: 8px; text-align: center; }
QProgressBar::chunk { background: %2; border-radius: 3px; }
QCheckBox::indicator {
    width: 14px; height: 14px;
    border: 1px solid palette(mid); border-radius: 3px;
    background: palette(base);
}
QCheckBox::indicator:checked {
    background: palette(highlight);
    border-color: palette(highlight);
    image: url(:/icons/check.svg);
}
QToolButton#transportPlay {
    background: palette(button);
    border: 1px solid palette(mid);
    border-radius: 19px;
    padding: 0;
}
QToolButton#transportPlay:hover { border-color: palette(highlight); }
QToolButton#transportPlay:pressed { background: palette(highlight); }
)")
        .arg(rgba(accent, 70), rgba(accent, 190), text.name());
}
} // namespace theme
