#include "links.h"
#include "mainwindow.h"
#include "settings.h"
#include "theme.h"
#include "updater.h"

#include <QApplication>
#include <QLibraryInfo>
#include <QTimer>
#include <QTranslator>

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

    // Переводчик ставим до создания окон: весь текст задаётся в конструкторах,
    // и более поздняя установка уже ничего не перепишет.
    //
    // Переводчики объявлены НЕ в блоке, а на весь main(): Qt снимает
    // переводчик, как только он уничтожен, - локальная переменная внутри if
    // отменила бы перевод сразу после выхода из блока.
    //
    // Свой файл переводит лаунчер, qtbase_* - стандартные кнопки Qt
    // (OK, Cancel, Yes/No), которые приходят из его библиотек, а не из нашего
    // кода, поэтому своим .qm их не покрыть.
    QTranslator launcherTranslator;
    QTranslator qtTranslator;
    const QString language = Settings::language();
    if (!language.isEmpty() && language != QLatin1String("en")) {
        if (launcherTranslator.load(QLatin1String(":/i18n/kogamaz_") + language
                                    + QLatin1String(".qm"))) {
            a.installTranslator(&launcherTranslator);
        } else {
            qWarning("kogamaz i18n: no translation for %s", qPrintable(language));
        }
        if (qtTranslator.load(QLatin1String("qtbase_") + language,
                              QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
            a.installTranslator(&qtTranslator);
        }
    }

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
    // Ручная проверка из меню Справка. Колбэк, а не прямой вызов: mainwindow.cpp
    // намеренно не знает про updater.cpp, иначе тестовым целям пришлось бы
    // тянуть Velopack, которую они не используют.
    w.setUpdateCheck([&w]() { Updater::check(&w, true); });
    w.show();
    Updater::check(&w);
    if (!link.isEmpty()) QTimer::singleShot(0, &w, [&w, link]() { w.openLink(link); });
    return QApplication::exec();
}
