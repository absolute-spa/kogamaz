#include "links.h"
#include "mainwindow.h"
#include "theme.h"
#include "updater.h"

#include <QApplication>
#include <QTimer>

int main(int argc, char *argv[])
{
    // QApplication создаём первым и без исключений: самозамена ниже берёт
    // applicationDirPath()/applicationFilePath(), а Qt требует для них инстанс
    // приложения (в QCoreApplicationPrivate::fileName() лежит
    // Q_ASSERT_X(QCoreApplication::self, ...)). В релизе assert вырезан, путь
    // получался пустым, и самозамена молча не копировала ни одного файла -
    // обновление «проходило», но ничего не меняло.
    QApplication a(argc, argv);
    // Имена organization/application задаём ДО чтения любых настроек: Paths::data()
    // строится из AppLocalDataLocation и без них указывает на другой каталог,
    // из-за чего тема читала бы не тот settings.ini.
    a.setOrganizationName("KoGaMaZ");
    a.setApplicationName("KoGaMaZ");

    // Самозамена портативной сборки: отдельный короткожизненный процесс,
    // который ждёт закрытия старого лаунчера, ставит файлы на место и
    // перезапускает. Должен сработать раньше темы - файлы не должны быть
    // заняты никем, кроме самой Qt.
    if (Updater::applyIfNeeded(argc, argv)) return 0;

    Updater::startup();
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
