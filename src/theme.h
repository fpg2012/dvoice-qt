// 主题适配：所有颜色从 QPalette 派生，跟随系统浅色/深色方案。
#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

class QPalette;

namespace theme {

bool isDark();
QColor tone(const QColor &color);

// 从资源里的 SVG 生成图标：把 currentColor 替换成 palette 的窗口文字色。
QIcon icon(const QString &name);

// 应用图标（自带配色，不随主题变化）。
QIcon appIcon();

// 调试用：强制切换整套调色板，之后再按 palette 重建样式表。
void applyScheme(bool dark);

// 只描述结构、颜色全部取 palette 的样式表。
QString styleSheet(const QPalette &palette);

} // namespace theme
