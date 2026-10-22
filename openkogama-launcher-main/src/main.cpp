#include "links.h"
#include "mainwindow.h"
#include "theme.h"
#include "updater.h"

#include <QApplication>
#include <QTimer>

int main(int argc, char *argv[])
{
    // Самозамена портативной сборки: отдельный короткийжизненный процесс,
    // который ждёт закрытия старого лаунчера, ставит файлы на место и
    // перезапускает. Должен сработать раньше QApplication и темы.
    if (Updater::applyIfNeeded(argc, argv)) return 0;

    Updater::startup();
    QApplication a(argc, argv);
    // Имена organization/application задаём ДО чтения любых настроек: Paths::data()
    // строится из AppLocalDataLocation и без них указывает на другой каталог,
    // из-за чего тема читала бы не тот settings.ini.
    a.setOrganizationName("KoGaMaZ");
    a.setApplicationName("KoGaMaZ");
    const QString link = Links::find(a.arguments());
    if (!link.isEmpty() && Links::forward(link)) return 0;
    a.setStyle(Theme::style());
    Theme::apply(a);
    Links::registerScheme();
    MainWindow w;
    Links::listen(&w, [&w](const QString &opened) { w.openLink(opened); });
    w.show();
    Updater::check(&w);
    if (!link.isEmpty()) QTimer::singleShot(0, &w, [&w, link]() { w.openLink(link); });
    return QApplication::exec();
}
