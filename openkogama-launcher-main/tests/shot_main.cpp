#include "mainwindow.h"
#include "settings.h"
#include "settingsdialog.h"
#include "theme.h"

#include <QApplication>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QTabWidget>

// Одноразовый харнесс: показывает главное окно и сохраняет его снимок,
// чтобы глазами проверить логотип, статистику и вкладку Profile.
int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName("KoGaMaZShots");
    QApplication::setApplicationName("Shot-" + QString::number(QApplication::applicationPid()));
    QStandardPaths::setTestModeEnabled(true);

    // Тема и шрифты - как в настоящем main.cpp, иначе снимок показывает
    // серый виджет без стилей, а не то, что увидит игрок.
    app.setStyle(Theme::style());
    Theme::apply(app);

    const QString socialUrl = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    if (!socialUrl.isEmpty()) Settings::setSocialUrl(socialUrl);

    // Снимок окна настроек: проверить, что на маленьком экране оно влезает
    // и листается, а не уводит кнопки за край.
    if (qEnvironmentVariableIsSet("SHOT_SETTINGS")) {
        SettingsDialog dialog;
        dialog.show();
        QTimer::singleShot(1200, [&]() {
            // SHOT_SCROLL=0..100 - куда промотать, в процентах
            if (auto *scroll = dialog.findChild<QScrollArea *>("settingsScroll"); scroll
                && qEnvironmentVariableIsSet("SHOT_SCROLL")) {
                QScrollBar *bar = scroll->verticalScrollBar();
                bar->setValue(bar->maximum() * qEnvironmentVariableIntValue("SHOT_SCROLL") / 100);
            }
            const QString out = qEnvironmentVariable("SHOT_OUT");
            dialog.grab().save(out.isEmpty() ? QStringLiteral("kogamaz-settings.png") : out);
            QTextStream(stdout) << "settings " << dialog.width() << "x" << dialog.height()
                                << " screen " << dialog.screen()->availableGeometry().width() << "x"
                                << dialog.screen()->availableGeometry().height() << "\n";
            app.quit();
        });
        return app.exec();
    }

    MainWindow window;
    if (!qEnvironmentVariableIsSet("SHOT_FIT")) window.resize(1180, 760);
    window.show();

    QTimer::singleShot(1200, [&]() {
        // Открываем вкладку Profile, если её просят отдельно
        if (qEnvironmentVariableIsSet("SHOT_PROFILE")) {
            auto *tabs = window.findChild<QTabWidget *>("mainTabs");
            for (int i = 0; i < tabs->count(); ++i)
                if (tabs->tabText(i) == QLatin1String("Profile")) tabs->setCurrentIndex(i);
        }
        QTimer::singleShot(700, [&]() {
            const QPixmap shot = window.grab();
            const QString out = qEnvironmentVariable("SHOT_OUT");
            const QString path = out.isEmpty() ? QStringLiteral("kogamaz-shot.png") : out;
            shot.save(path);

            // Дамп подписей: по картинке не понять, какой именно лейбл что
            // показывает, а для отладки важны имена и геометрия.
            const QString dumpPath = qEnvironmentVariable("SHOT_DUMP");
            if (!dumpPath.isEmpty()) {
                QFile dump(dumpPath);
                if (dump.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    QTextStream stream(&dump);
                    // Геометрия layout'ов шапки: логотип висит по центру, хотя
                    // в .ui он первый элемент - надо увидеть реальное выравнивание.
                    const auto layouts = window.findChildren<QLayout *>();
                    for (const QLayout *layout : layouts) {
                        const auto *box = qobject_cast<const QHBoxLayout *>(layout);
                        if (!box) continue;
                        const QString name = layout->objectName();
                        if (name != QLatin1String("brandLayout")
                            && name != QLatin1String("headerLayout")) continue;
                        stream << "LAYOUT " << name
                               << " alignment=" << int(layout->alignment())
                               << " count=" << layout->count() << "\n";
                        for (int i = 0; i < layout->count(); ++i) {
                            const QLayoutItem *item = layout->itemAt(i);
                            const auto *w = item->widget();
                            const QRect g = w ? w->geometry() : QRect();
                            stream << "   item " << i << " align=" << int(item->alignment())
                                   << " geom=" << g.x() << "," << g.y() << " "
                                   << g.width() << "x" << g.height()
                                   << " name=" << (w ? w->objectName() : QString("<layout>"))
                                   << (w && w->isVisibleTo(&window) ? "" : " (hidden)") << "\n";
                        }
                    }

                    // Все именованные виджеты верхней части окна: по одной
                    // картинке непонятно, что именно стоит по центру сверху.
                    const auto widgets = window.findChildren<QWidget *>();
                    for (const QWidget *widget : widgets) {
                        if (widget->objectName().isEmpty()) continue;
                        const QPoint topLeft = widget->mapTo(&window, QPoint(0, 0));
                        QStringList chain;
                        for (const QObject *p = widget->parent(); p; p = p->parent()) {
                            const auto *w = qobject_cast<const QWidget *>(p);
                            if (w && !w->objectName().isEmpty()) chain.prepend(w->objectName());
                            if (w == &window) break;
                        }
                        stream << widget->metaObject()->className() << " "
                               << widget->objectName() << " | " << chain.join('>')
                               << " | " << topLeft.x() << "," << topLeft.y() << " "
                               << widget->width() << "x" << widget->height()
                               << (widget->isVisibleTo(&window) ? "" : " (hidden)") << "\n";
                    }
                }
            }
            app.quit();
        });
    });
    return app.exec();
}
