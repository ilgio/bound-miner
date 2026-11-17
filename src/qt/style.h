#pragma once

// The wallet app's look: Qt's Fusion style with BOUND's colours, light or dark to match the
// system (and switching when the system does, milestone 18), and the logo (compiled in from
// bound.qrc).

#include <QColor>
#include <QFont>
#include <QFrame>
#include <QIcon>
#include <QLabel>
#include <QPixmap>
#include <QString>

#include <functional>

class QApplication;
class QObject;

namespace bound::gui {

struct Colors {
    bool dark = false;
    QColor window, card, text, muted, border, accent, accent_soft, accent_text, hover, positive, negative, warning_bg,
        warning_border, sidebar, sidebar_text, sidebar_selected;
};

// Sets the style, palette and style sheet; call once, before any window is made.
void apply_style(QApplication& app);
const Colors& colors();
// Milestone 18: `listener` runs after the colours change with the system's theme, while `owner`
// lives. For what a page painted with colors() itself.
void on_theme_change(QObject* owner, std::function<void()> listener);

QPixmap logo(int size);        // the app icon: the white mark on navy, a rounded square
QPixmap white_mark(int size);  // the mark alone, white, for dark backgrounds
QIcon app_icon();
QFont mono_font(int point_size = 0);  // 0: the application's size

// Building blocks of the pages.
QFrame* card(QWidget* parent = nullptr);          // a rounded panel
QLabel* page_title(const QString& text, QWidget* parent = nullptr);
QLabel* muted_label(const QString& text, QWidget* parent = nullptr);
QLabel* warning_banner(const QString& text, QWidget* parent = nullptr);

}  // namespace bound::gui
