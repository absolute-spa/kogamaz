#ifndef UPDATECHECK_H
#define UPDATECHECK_H

#include <QByteArray>
#include <QString>

class QNetworkAccessManager;

// Проверка обновлений через публичный релиз GitHub.
//
// Токен не нужен и не зашивается в exe: публичные релизы читаются без
// авторизации (60 запросов в час с IP - для проверки при каждом запуске
// хватает с запасом). Секрет требуется только для публикации релиза, и он
// живёт в Actions Secrets, а не здесь.
namespace UpdateCheck {

// Репозиторий, откуда берутся релизы. Владелец меняет его перед раздачей.
QString repository();

struct Release {
    QString version;    // "1.2.0" - без ведущей v
    QString tag;        // "v1.2.0" - как подписан релиз
    QString title;      // имя релиза
    QString notes;      // описание: что изменилось
    QString zipUrl;     // прямая ссылка на zip-артефакт, если есть
    QString htmlUrl;    // страница релиза в браузере
    bool isValid() const { return !version.isEmpty(); }
};

// Разбирает тело ответа GitHub API. Понимает и /releases/latest (объект),
// и /releases (массив - берётся первый не-предрелиз). Пустой результат =
// релизов нет или ответ непонятный, ошибку показывать игроку незачем.
Release parse(const QByteArray &json);

// Строит URL последнего релиза для запроса.
QString latestUrl();

// Адрес, по которому лаунчер узнаёт о новой версии. Вынесено отдельно, чтобы
// тесты подменяли его на локальный сервер вместо GitHub.
void setEndpoint(const QString &url);

}

#endif // UPDATECHECK_H
