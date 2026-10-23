#include "theme.h"
#include "settings.h"
#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QGuiApplication>
#include <QProxyStyle>
#include <QScreen>
#include <QWidget>

#include <algorithm>

namespace {

class PlainStyle : public QProxyStyle
{
public:
    PlainStyle() : QProxyStyle("Fusion") {}

    int styleHint(StyleHint hint, const QStyleOption *option, const QWidget *widget, QStyleHintReturn *data) const override
    {
        return hint == SH_UnderlineShortcut ? 0 : QProxyStyle::styleHint(hint, option, widget, data);
    }
};

// KoGaMaZ: dark palette, TLauncher-inspired. The accent and its hover/pressed
// variants are derived from the user-chosen accent color in settings; the rest
// of the surface colors are fixed dark neutrals.
const QHash<QString, QString> Palette = {
    {"background", "#12161d"},
    {"panel", "#181e29"},
    {"text", "#f2f5fa"},
    {"muted", "#8e98a9"},
    {"button", "#222a38"},
    {"border", "#2d3747"},
    {"hover", "#2b3546"},
    {"hover-border", "#3d4c63"},
    {"pressed", "#1a212c"},
    {"disabled", "#191f2a"},
    {"disabled-text", "#4e5869"},
    {"disabled-border", "#232b38"},
    {"field", "#1a212c"},
    {"alternate", "#161d27"},
    {"separator", "#252e3d"},
    {"scroll", "#333e50"},
    {"scroll-hover", "#455369"},
    {"check-border", "#44526a"},
    {"plus", ":/plus.svg"},
    {"arrow", ":/arrow-down.svg"},
};

QString styleSheet() {
    QFile file(":/launcher.qss");
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QString style = QString::fromUtf8(file.readAll());

    // Accent family is dynamic: base from settings, hover lighter, pressed darker.
    const QColor accent(Settings::accentColor());
    const QString accentHex = accent.name(QColor::HexRgb);
    const QString accentHover = accent.lighter(118).name(QColor::HexRgb);
    const QString accentPressed = accent.darker(120).name(QColor::HexRgb);

    QHash<QString, QString> palette = Palette;
    palette.insert("accent", accentHex);
    palette.insert("accent-hover", accentHover);
    palette.insert("accent-pressed", accentPressed);

    for (auto it = palette.begin(); it != palette.end(); ++it)
        style.replace("@" + it.key() + "@", it.value());
    return style;
}

}

void Theme::apply(QApplication &app) {
    app.setStyleSheet(styleSheet());
}

void Theme::reapply() {
    if (auto *app = qobject_cast<QApplication *>(QCoreApplication::instance()))
        app->setStyleSheet(styleSheet());
}

QStyle *Theme::style() {
    return new PlainStyle;
}

bool Theme::dark() {
    return true;
}

void Theme::fitToScreen(QWidget *window, const QSize &preferred) {
    if (!window) return;
    QWidget *anchor = window->parentWidget() ? window->parentWidget()->window() : nullptr;
    QScreen *screen = anchor ? anchor->screen() : window->screen();
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (!screen) {
        window->resize(preferred);
        return;
    }

    // Запас под рамку и заголовок окна: availableGeometry их не учитывает.
    const QRect area = screen->availableGeometry();
    const QSize room(area.width() * 92 / 100, area.height() * 92 / 100);
    window->resize(preferred.boundedTo(room).expandedTo(window->minimumSize().boundedTo(room)));

    // Центрируем по родителю, но так, чтобы окно целиком осталось на экране.
    const QPoint center = anchor ? anchor->frameGeometry().center() : area.center();
    QRect frame(QPoint(0, 0), window->size());
    frame.moveCenter(center);
    frame.moveLeft(std::clamp(frame.left(), area.left(), std::max(area.left(), area.right() - frame.width())));
    frame.moveTop(std::clamp(frame.top(), area.top(), std::max(area.top(), area.bottom() - frame.height())));
    window->move(frame.topLeft());
}
