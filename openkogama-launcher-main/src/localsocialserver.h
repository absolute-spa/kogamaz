#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

class QUrlQuery;
class QTcpServer;
class QTcpSocket;
class LocalSocialStore;

// Встроенный соц-сервер KoGaMaZ.
//
// Раньше соц-функции требовали отдельный Node-сервер, который надо было ставить
// и где-то запускать - из-за этого у друзей регистрация просто не работала.
// Теперь сервер живёт прямо в лаунчере: он слушает 127.0.0.1 на случайном
// свободном порту и хранит аккаунты в JSON-файле рядом с остальными данными.
// Внешний интернет и установка ничего не требуют.
//
// API совпадает с social-server/ по сути и по кодам ошибок, поэтому клиент
// (social.cpp) не различает, отвечает встроенный сервер или удалённый.
class LocalSocialServer : public QObject
{
    Q_OBJECT

public:
    explicit LocalSocialServer(QObject *parent = nullptr);
    ~LocalSocialServer() override;

    // Поднимает сервер и возвращает базовый URL вида http://127.0.0.1:53411.
    // Пустая строка - не удалось слушать сокет (крайне маловероятно локально).
    QString start();

    bool isRunning() const;
    quint16 port() const;

    // Полная остановка - на выключении приложения, чтобы не держать сокет.
    void stop();

    // Ответ одним JSON-объектом; им пользуются обработчики маршрутов.
    static void respondJson(QTcpSocket *socket, int status, const QJsonObject &body);

signals:
    // Ошибка, которую игрок должен увидеть: не удалось сохранить файл данных
    // или занять порт. Дальше лаунчер продолжает работать без соц-функций.
    void failed(const QString &reason);

private:
    static void respond(QTcpSocket *socket, int status, const QByteArray &json);

    void handleConnection(QTcpSocket *socket);
    void route(const QString &method, const QString &path, const QUrlQuery &query,
               const QByteArray &body, const QByteArray &token, QTcpSocket *socket);
    // Люди и дружба: поиск, список друзей, заявки, присутствие, hosting.
    void routePeople(const QString &method, const QString &path, const QUrlQuery &query,
                     const QJsonObject &sent, const QByteArray &token, QTcpSocket *socket);
    // Личные сообщения: история, отправка, отметка прочтения, счётчик непрочитанных.
    void routeChat(const QString &method, const QString &path, const QUrlQuery &query,
                   const QJsonObject &sent, const QByteArray &token, QTcpSocket *socket);
    // Посты профиля: лента, добавление, удаление своего.
    void routePosts(const QString &method, const QString &path, const QUrlQuery &query,
                    const QJsonObject &sent, const QByteArray &token, QTcpSocket *socket);
    // Роли соавторов карты: список, приглашение, снятие роли.
    void routeWorlds(const QString &method, const QString &path, const QUrlQuery &query,
                     const QJsonObject &sent, const QByteArray &token, QTcpSocket *socket);

    QTcpServer *m_server = nullptr;
    LocalSocialStore *m_store = nullptr;
};
