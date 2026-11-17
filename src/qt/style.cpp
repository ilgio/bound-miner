#include "qt/style.h"

#include <QApplication>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QPalette>
#include <QPointer>
#include <QStyleFactory>
#include <QStyleHints>

#include <functional>
#include <utility>
#include <vector>

// The images in bound.qrc are compiled into this static library; this makes sure they're
// registered (Q_INIT_RESOURCE has to be used outside any namespace).
static void init_resources() { Q_INIT_RESOURCE(bound); }

namespace bound::gui {

namespace {

Colors g_colors;

bool system_is_dark() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
    if (scheme != Qt::ColorScheme::Unknown) return scheme == Qt::ColorScheme::Dark;
#endif
    return QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
}

Colors make_colors(bool dark) {
    Colors c;
    c.dark = dark;
    c.sidebar = QColor("#171a33");
    c.sidebar_text = QColor("#c5c9e6");
    c.sidebar_selected = QColor("#4c6ef5");
    c.accent_text = QColor("#ffffff");
    if (dark) {
        c.window = QColor("#111324");
        c.card = QColor("#1b1e34");
        c.text = QColor("#e6e8f2");
        c.muted = QColor("#9097b0");
        c.border = QColor("#2b2f4d");
        c.accent = QColor("#748ffc");
        c.accent_soft = QColor("#252b52");
        c.hover = QColor("#232741");
        c.positive = QColor("#69db7c");
        c.negative = QColor("#ff8787");
        c.warning_bg = QColor("#3b2f12");
        c.warning_border = QColor("#7a5a12");
    } else {
        c.window = QColor("#f3f4f9");
        c.card = QColor("#ffffff");
        c.text = QColor("#1c1e2b");
        c.muted = QColor("#686d84");
        c.border = QColor("#e1e4ee");
        c.accent = QColor("#4263eb");
        c.accent_soft = QColor("#edf1ff");
        c.hover = QColor("#f5f6fb");
        c.positive = QColor("#2b8a3e");
        c.negative = QColor("#c92a2a");
        c.warning_bg = QColor("#fff4e6");
        c.warning_border = QColor("#ffc078");
    }
    return c;
}

std::vector<std::pair<QPointer<QObject>, std::function<void()>>> g_theme_listeners;

QPalette make_palette(const Colors& c) {
    QPalette p;
    p.setColor(QPalette::Window, c.window);
    p.setColor(QPalette::WindowText, c.text);
    p.setColor(QPalette::Base, c.card);
    p.setColor(QPalette::AlternateBase, c.dark ? c.window.lighter(115) : c.window);
    p.setColor(QPalette::Text, c.text);
    p.setColor(QPalette::PlaceholderText, c.muted);
    p.setColor(QPalette::Button, c.card);
    p.setColor(QPalette::ButtonText, c.text);
    p.setColor(QPalette::ToolTipBase, c.card);
    p.setColor(QPalette::ToolTipText, c.text);
    p.setColor(QPalette::Highlight, c.accent);
    p.setColor(QPalette::HighlightedText, c.accent_text);
    p.setColor(QPalette::Link, c.accent);
    p.setColor(QPalette::Mid, c.border);
    p.setColor(QPalette::Dark, c.border.darker(130));
    p.setColor(QPalette::Light, c.card.lighter(110));
    p.setColor(QPalette::Disabled, QPalette::Text, c.muted);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, c.muted);
    p.setColor(QPalette::Disabled, QPalette::WindowText, c.muted);
    return p;
}

QString style_sheet(const Colors& c) {
    const auto n = [](const QColor& color) { return color.name(); };
    return QStringLiteral(R"(
QWidget#content { background: %1; }
QListWidget#sidebar { background: %2; border: none; color: %3; outline: 0; padding-top: 6px; }
QListWidget#sidebar::item { padding: 9px 12px; margin: 2px 10px; border-radius: 8px; }
QListWidget#sidebar::item:hover { background: rgba(255, 255, 255, 0.07); }
QListWidget#sidebar::item:selected { background: %4; color: white; }
QFrame#txRow { border-radius: 8px; }
QFrame#txRow:hover { background: %11; }
QFrame#qrBox { background: white; border: 1px solid %6; border-radius: 10px; }
QPushButton:checked { background: %12; border-color: %4; color: %13; font-weight: 600; }
QToolButton { padding: 6px 10px; border-radius: 7px; border: 1px solid %6; background: %5; }
QToolButton:hover { border-color: %4; }
QToolButton::menu-indicator { image: none; width: 0; }
QRadioButton { spacing: 8px; padding: 2px 0; }
QWidget#brand { background: %2; }
QLabel#brandName { color: white; font-size: 18px; font-weight: 700; }
QLabel#pill { background: #f08c00; color: white; border-radius: 8px; padding: 1px 8px; font-size: 10px; font-weight: 700; }
QFrame#card { background: %5; border: 1px solid %6; border-radius: 12px; }
QLabel#pageTitle { font-size: 22px; font-weight: 700; }
QLabel#cardTitle { font-size: 12px; font-weight: 700; color: %7; }
QLabel#bigAmount { font-size: 30px; font-weight: 700; }
QLabel#mediumValue { font-size: 18px; font-weight: 600; }
QLabel#muted { color: %7; }
QLabel#banner { background: %8; border: 1px solid %9; border-radius: 8px; padding: 10px 12px; }
QPushButton { padding: 7px 16px; border-radius: 7px; border: 1px solid %6; background: %5; }
QPushButton:hover { border-color: %4; }
QPushButton:disabled { color: %7; }
QPushButton#primary { background: %4; color: white; border: 1px solid %4; font-weight: 600; }
QPushButton#primary:hover { background: %10; }
QPushButton#primary:disabled { background: %6; border-color: %6; color: %7; }
QLineEdit, QPlainTextEdit, QTextEdit, QSpinBox, QComboBox { border: 1px solid %6; border-radius: 7px; padding: 6px 8px; background: %5; }
QLineEdit:focus, QPlainTextEdit:focus, QComboBox:focus { border-color: %4; }
QTableWidget { border: 1px solid %6; border-radius: 10px; background: %5; gridline-color: transparent; }
QTableWidget::item { padding: 4px 8px; }
QHeaderView::section { background: %5; border: none; border-bottom: 1px solid %6; padding: 6px 8px; font-weight: 600; color: %7; }
QStatusBar { background: %5; border-top: 1px solid %6; }
QStatusBar QLabel { padding: 0 8px; }
QTabWidget::pane { border: 1px solid %6; border-radius: 10px; top: -1px; background: %5; }
QTabBar::tab { padding: 7px 16px; border: none; color: %7; }
QTabBar::tab:selected { color: %4; border-bottom: 2px solid %4; }
QProgressBar { border: 1px solid %6; border-radius: 5px; background: %1; height: 10px; text-align: center; }
QProgressBar::chunk { background: %4; border-radius: 4px; }
)")
        .arg(n(c.window), n(c.sidebar), n(c.sidebar_text), n(c.sidebar_selected), n(c.card), n(c.border), n(c.muted),
             n(c.warning_bg), n(c.warning_border))
        .arg(n(c.sidebar_selected.lighter(115)), n(c.hover), n(c.accent_soft), n(c.accent));
}

void restyle(QApplication& app, bool dark) {
    g_colors = make_colors(dark);
    QApplication::setPalette(make_palette(g_colors));
    app.setStyleSheet(style_sheet(g_colors));
}

}  // namespace

void apply_style(QApplication& app) {
    init_resources();
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    restyle(app, system_is_dark());
    app.setWindowIcon(app_icon());
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    // Milestone 18: follow Windows when it switches between light and dark, without a restart.
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, &app, [&app](Qt::ColorScheme scheme) {
        if (scheme == Qt::ColorScheme::Unknown || (scheme == Qt::ColorScheme::Dark) == g_colors.dark) return;
        restyle(app, scheme == Qt::ColorScheme::Dark);
        std::erase_if(g_theme_listeners, [](const auto& l) { return l.first.isNull(); });
        for (const auto& [owner, listener] : g_theme_listeners) listener();
    });
#endif
}

const Colors& colors() { return g_colors; }

void on_theme_change(QObject* owner, std::function<void()> listener) { g_theme_listeners.emplace_back(owner, std::move(listener)); }

namespace {

// An image from the app's resources (bound.qrc), `size` pixels on screen, sharp on high-DPI screens.
QPixmap resource_image(const QString& name, int size) {
    const qreal dpr = qApp != nullptr ? qApp->devicePixelRatio() : 1.0;
    QPixmap pixmap = QPixmap(QStringLiteral(":/bound/") + name)
                         .scaled(QSize(size, size) * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    pixmap.setDevicePixelRatio(dpr);
    return pixmap;
}

}  // namespace

QPixmap logo(int size) { return resource_image(QStringLiteral("icon.png"), size); }

QPixmap white_mark(int size) { return resource_image(QStringLiteral("logo-white.png"), size); }

QIcon app_icon() { return QIcon(QStringLiteral(":/bound/icon.png")); }

QFont mono_font(int point_size) {
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (point_size > 0) font.setPointSize(point_size);
    return font;
}

QFrame* card(QWidget* parent) {
    auto* frame = new QFrame(parent);
    frame->setObjectName(QStringLiteral("card"));
    return frame;
}

QLabel* page_title(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("pageTitle"));
    return label;
}

QLabel* muted_label(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("muted"));
    label->setWordWrap(true);
    return label;
}

QLabel* warning_banner(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("banner"));
    label->setWordWrap(true);
    return label;
}

}  // namespace bound::gui
