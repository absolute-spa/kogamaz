#include "localsocialserver.h"
#include "social.h"
#include "settings.h"
#include "paths.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <functional>

// End-to-end проверка встроенного соц-сервера: поднимаем настоящий сервер и
// ходим в него настоящим клиентом social.cpp. Это ровно тот путь, которым
// регистрация работает у игрока - моки тут не подходили бы, потому что они
// проверяют только клиент, а не контракт сервера.

namespace {

void spinUntil(const std::function<bool()> &done, int timeoutMs = 8000)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

// Ожидает ответа ровно одной операции клиента и возвращает его.
QJsonObject call(const std::function<void(const Social::Callback &)> &op, bool *ok = nullptr)
{
    bool result = false;
    QJsonObject payload;
    QString error;
    op([&](bool success, const QJsonObject &json, const QString &why) {
        result = success;
        payload = json;
        error = why;
    });
    spinUntil([&]() { return !error.isEmpty() || payload.contains("token")
        || payload.contains("results") || payload.contains("friends")
        || payload.contains("incoming") || payload.contains("messages")
        || payload.contains("posts") || payload.contains("unread")
        || payload.contains("members") || payload.contains("hosting")
        || payload.contains("ok"); });
    if (ok) *ok = result;
    return payload;
}

// Ответ прямого запроса: код и тело. Код нужен, чтобы проверять отказы
// (401/403/404), а не только успешные ответы.
struct RawReply {
    int status = 0;
    QJsonObject body;
};

// Прямой запрос к серверу: нужен для служебных путей (/api/health, /api/unread)
// и для маршрутов, которыми клиент social.cpp пока не оборачивает. События
// крутим через QEventLoop, а не waitForReadyRead: сервер живёт в том же
// потоке, и пока мы блокируемся жёстко, у него нет шанса прочитать запрос.
RawReply httpRequest(const QString &method, const QString &path,
                     const QByteArray &body = QByteArray(),
                     const QString &token = QString())
{
    RawReply result;
    const QUrl url(Settings::socialUrl() + path);
    QTcpSocket socket;
    socket.connectToHost(url.host(), quint16(url.port(80)));

    QByteArray raw;
    QEventLoop loop;
    QObject::connect(&socket, &QTcpSocket::connected, &socket, [&]() {
        QByteArray head = method.toLatin1() + " " + path.toUtf8() + " HTTP/1.1\r\nHost: "
            + url.host().toUtf8() + "\r\nConnection: close\r\n";
        if (!body.isEmpty()) {
            head += "Content-Type: application/json\r\n"
                    "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
        }
        if (!token.isEmpty())
            head += "Authorization: Bearer " + token.toUtf8() + "\r\n";
        socket.write(head + "\r\n" + body);
    });
    QObject::connect(&socket, &QTcpSocket::readyRead, &socket, [&]() { raw += socket.readAll(); });
    QObject::connect(&socket, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();

    raw += socket.readAll();
    const int split = raw.indexOf("\r\n\r\n");
    if (split >= 0) {
        // "HTTP/1.1 403 Forbidden"
        const QList<QByteArray> statusLine = raw.left(raw.indexOf("\r\n")).split(' ');
        if (statusLine.size() >= 2) result.status = statusLine.at(1).toInt();
        result.body = QJsonDocument::fromJson(raw.mid(split + 4)).object();
    }
    return result;
}

// Обёртка для путей, где важен только тело.
QJsonObject httpGet(const QString &path, const QString &token = QString())
{
    return httpRequest(QStringLiteral("GET"), path, QByteArray(), token).body;
}

// POST с JSON-телом.
RawReply httpPost(const QString &path, const QJsonObject &payload, const QString &token = QString())
{
    return httpRequest(QStringLiteral("POST"), path,
                       QJsonDocument(payload).toJson(QJsonDocument::Compact), token);
}

} // namespace

class LocalSocialTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // Данные - в тестовой папке QStandardPaths, чтобы не трогать реальные
        // аккаунты игрока в %LOCALAPPDATA%. Переключаем режим до первого
        // обращения к Paths::data().
        QStandardPaths::setTestModeEnabled(true);

        // База с прошлого прогона пережила бы перезапуск - и регистрация
        // упёрлась бы в 409 вместо чистого старта.
        QFile::remove(Paths::data() + "/social.json");

        m_data = new QTemporaryDir;
        QVERIFY(m_data->isValid());

        m_server = new LocalSocialServer;
        m_url = m_server->start();
        QVERIFY2(m_server->isRunning(), "the built-in server must listen");
        QVERIFY(m_url.startsWith("http://127.0.0.1:"));
        Settings::setSocialUrl(m_url);
    }

    void cleanupTestCase()
    {
        Settings::setSocialUrl(QString());
        Settings::setSocialToken(QString());
        delete m_server;
        delete m_data;
    }

    void health()
    {
        // /api/health клиентом не обёрнут (это служебный пинг), поэтому
        // дёргаем его напрямую простым HTTP-запросом на тот же адрес.
        const QJsonObject body = httpGet("/api/health");
        QCOMPARE(body.value("ok").toBool(), true);
        QCOMPARE(body.value("backend").toString(), QString("local"));
    }

    void registerAndLogin()
    {
        bool ok = false;
        QJsonObject body = call([&](const Social::Callback &done) {
            Social::signUp("FeelZ Dev", "feelz", "hunter22", done);
        }, &ok);
        QVERIFY2(ok, "registration against the built-in server must work");
        m_aliceId = body.value("id").toString();
        m_aliceToken = body.value("token").toString();
        // Клиент берёт токен из настроек - без этого /api/me отвечает 401,
        // как и было бы у настоящего игрока до входа.
        Settings::setSocialToken(m_aliceToken);
        Settings::setSocialId(m_aliceId);
        Settings::setSocialHandle(body.value("handle").toString());
        QCOMPARE(body.value("nick").toString(), QString("FeelZ Dev"));
        QCOMPARE(body.value("handle").toString(), QString("feelz"));
        QCOMPARE(body.value("level").toInt(), 1);

        // Повторная регистрация того же @username отклоняется
        ok = true;
        body = call([&](const Social::Callback &done) {
            Social::signUp("FeelZ Dev 2", "feelz", "hunter22", done);
        }, &ok);
        QVERIFY(!ok);

        // Дубль ника тоже (ник сравнивается без учёта регистра)
        ok = true;
        body = call([&](const Social::Callback &done) {
            Social::signUp("feelz dev", "feelz2", "hunter22", done);
        }, &ok);
        QVERIFY(!ok);

        // Неверный пароль - отказ, а не сессия
        ok = true;
        call([&](const Social::Callback &done) {
            Social::login("feelz", "wrongpass", done);
        }, &ok);
        QVERIFY(!ok);

        // Вход по @username
        ok = false;
        body = call([&](const Social::Callback &done) {
            Social::login("@feelz", "hunter22", done);
        }, &ok);
        QVERIFY2(ok, "login by @username must work");
        QCOMPARE(body.value("nick").toString(), QString("FeelZ Dev"));
    }

    void meAfterLogin()
    {
        bool ok = false;
        const QJsonObject body = call([](const Social::Callback &done) { Social::me(done); }, &ok);
        QVERIFY(ok);
        QCOMPARE(body.value("handle").toString(), QString("feelz"));
        QCOMPARE(body.value("level").toInt(), 1);
    }

    void secondAccountAndSearch()
    {
        bool ok = false;
        QJsonObject body = call([&](const Social::Callback &done) {
            Social::signUp("Bob", "bobdev", "hunter22", done);
        }, &ok);
        QVERIFY(ok);
        m_bobId = body.value("id").toString();
        m_bobToken = body.value("token").toString();

        // Поиск по @username отдаёт точное совпадение первым
        ok = false;
        const QJsonObject results = call([&](const Social::Callback &done) {
            Social::search("bobdev", done);
        }, &ok);
        QVERIFY(ok);
        const QJsonArray rows = results.value("results").toArray();
        QVERIFY(!rows.isEmpty());
        QCOMPARE(rows.first().toObject().value("id").toString(), m_bobId);
        QCOMPARE(rows.first().toObject().value("relation").toString(), QString("none"));
    }

    void friendRequestFlow()
    {
        // Переключаемся на Алису: у неё токен её сессии
        Settings::setSocialToken(m_aliceToken);
        Settings::setSocialId(m_aliceId);

        bool ok = false;
        call([&](const Social::Callback &done) { Social::sendRequest(m_bobId, done); }, &ok);
        QVERIFY2(ok, "a friend request must be accepted");

        // Повторная заявка - конфликт
        ok = true;
        call([&](const Social::Callback &done) { Social::sendRequest(m_bobId, done); }, &ok);
        QVERIFY(!ok);

        // У Боба заявка видна как входящая
        Settings::setSocialToken(m_bobToken);
        Settings::setSocialId(m_bobId);
        ok = false;
        QJsonObject lists = call([](const Social::Callback &done) { Social::requests(done); }, &ok);
        QVERIFY(ok);
        const QJsonArray incoming = lists.value("incoming").toArray();
        QCOMPARE(incoming.size(), 1);
        QCOMPARE(incoming.first().toObject().value("nick").toString(), QString("FeelZ Dev"));

        // Поиск у Алисы теперь показывает исходящую заявку
        Settings::setSocialToken(m_aliceToken);
        ok = false;
        lists = call([](const Social::Callback &done) { Social::search("bobdev", done); }, &ok);
        QVERIFY(ok);
        QCOMPARE(lists.value("results").toArray().first().toObject()
                     .value("relation").toString(), QString("outgoing"));
    }

    void acceptMakesFriends()
    {
        Settings::setSocialToken(m_bobToken);
        bool ok = false;
        QJsonObject lists = call([](const Social::Callback &done) { Social::requests(done); }, &ok);
        QVERIFY(ok);
        const QString requestId = lists.value("incoming").toArray()
                                      .first().toObject().value("id").toString();

        ok = false;
        call([&](const Social::Callback &done) { Social::answer(requestId, "accept", done); }, &ok);
        QVERIFY(ok);

        // Заявка снята, дружба установлена с обеих сторон
        ok = false;
        lists = call([](const Social::Callback &done) { Social::requests(done); }, &ok);
        QVERIFY(ok);
        QVERIFY(lists.value("incoming").toArray().isEmpty());

        ok = false;
        lists = call([](const Social::Callback &done) { Social::friends(done); }, &ok);
        QVERIFY(ok);
        QCOMPARE(lists.value("friends").toArray().size(), 1);
        QCOMPARE(lists.value("friends").toArray().first().toObject()
                     .value("handle").toString(), QString("feelz"));
    }

    void chatBetweenFriends()
    {
        Settings::setSocialToken(m_aliceToken);
        bool ok = false;
        QJsonObject body = call([&](const Social::Callback &done) {
            Social::sendMessage(m_bobId, "privet!", done);
        }, &ok);
        QVERIFY2(ok, "friends must be able to message each other");
        QCOMPARE(body.value("mine").toBool(), true);

        // История видна получателю и содержит ровно наше сообщение
        Settings::setSocialToken(m_bobToken);
        ok = false;
        body = call([&](const Social::Callback &done) {
            Social::chatHistory(m_aliceId, 0, done);
        }, &ok);
        QVERIFY(ok);
        const QJsonArray messages = body.value("messages").toArray();
        QCOMPARE(messages.size(), 1);
        QCOMPARE(messages.first().toObject().value("body").toString(), QString("privet!"));
        QCOMPARE(messages.first().toObject().value("mine").toBool(), false);

        // Счётчик непрочитанных у получателя
        const QJsonObject unread = httpGet("/api/unread", m_bobToken);
        QCOMPARE(unread.value("unread").toArray().size(), 1);
        QCOMPARE(unread.value("unread").toArray().first().toObject()
                     .value("player").toString(), m_aliceId);
        QCOMPARE(unread.value("unread").toArray().first().toObject()
                     .value("count").toInt(), 1);

        // Прочитали - счётчик обнуляется
        Settings::setSocialToken(m_bobToken);
        bool read = false;
        call([&](const Social::Callback &done) {
            Social::markRead(m_aliceId, done);
        }, &read);
        QVERIFY(read);
        QVERIFY(httpGet("/api/unread", m_bobToken).value("unread").toArray().isEmpty());
    }

    void postsFlow()
    {
        Settings::setSocialToken(m_aliceToken);
        bool ok = false;
        QJsonObject body = call([&](const Social::Callback &done) {
            Social::addPost("first post!", done);
        }, &ok);
        QVERIFY2(ok, "adding a post must work");
        const QString postId = body.value("id").toString();
        QCOMPARE(body.value("body").toString(), QString("first post!"));

        // Своя лента отдаёт пост и поля владельца профиля
        ok = false;
        body = call([](const Social::Callback &done) { Social::posts("", done); }, &ok);
        QVERIFY(ok);
        QCOMPARE(body.value("posts").toArray().size(), 1);
        QCOMPARE(body.value("nick").toString(), QString("FeelZ Dev"));
        QCOMPARE(body.value("player").toString(), m_aliceId);

        // Друг чужой профиль читает
        Settings::setSocialToken(m_bobToken);
        ok = false;
        body = call([&](const Social::Callback &done) {
            Social::posts(m_aliceId, done);
        }, &ok);
        QVERIFY2(ok, "a friend must be able to read the profile posts");
        QCOMPARE(body.value("posts").toArray().size(), 1);

        // Удаление чужого поста отвечает успехом, но пост остаётся
        ok = false;
        call([&](const Social::Callback &done) { Social::deletePost(postId, done); }, &ok);
        QVERIFY(ok);
        ok = false;
        body = call([&](const Social::Callback &done) {
            Social::posts(m_aliceId, done);
        }, &ok);
        QVERIFY(ok);
        QCOMPARE(body.value("posts").toArray().size(), 1);

        // Своё удалить - можно
        Settings::setSocialToken(m_aliceToken);
        ok = false;
        call([&](const Social::Callback &done) { Social::deletePost(postId, done); }, &ok);
        QVERIFY(ok);
        ok = false;
        body = call([](const Social::Callback &done) { Social::posts("", done); }, &ok);
        QVERIFY(ok);
        QVERIFY(body.value("posts").toArray().isEmpty());
    }

    void levelGrowsWithPosts()
    {
        // Уровень на сервере не хранится - он считается из опыта по формуле
        // L^2 * 100. Проверяем, что опыт за посты действительно двигает
        // уровень вверх и шапка увидит его через /api/me.
        bool ok = false;
        QJsonObject body = call([&](const Social::Callback &done) {
            Social::signUp("Level Tester", "levelup", "hunter22", done);
        }, &ok);
        QVERIFY2(ok, "registration must work");
        Settings::setSocialToken(body.value("token").toString());
        QCOMPARE(body.value("level").toInt(), 1);
        QCOMPARE(static_cast<qint64>(body.value("exp").toDouble()), qint64(0));

        // 100 exp = 10 постов по 10 exp - ровно порог второго уровня.
        for (int i = 0; i < 10; ++i) {
            ok = false;
            call([&](const Social::Callback &done) { Social::addPost("post " + QString::number(i), done); }, &ok);
            QVERIFY2(ok, "each post must be accepted");
        }

        ok = false;
        body = call([](const Social::Callback &done) { Social::me(done); }, &ok);
        QVERIFY2(ok, "/api/me must report the fresh stats");
        QCOMPARE(static_cast<qint64>(body.value("exp").toDouble()), qint64(100));
        QCOMPARE(body.value("level").toInt(), 2);

        Settings::setSocialToken(QString());
    }

    void worldRoles()
    {
        // Роли соавторов карты. Живут только в лаунчере, но контракт обязан
        // совпадать с Node-сервером: в проде отвечает именно он, и клиент не
        // должен заметить разницы. Игроков заводим здесь, чтобы тест не
        // зависел от порядка остальных тестов.
        auto join = [](const QString &nick, const QString &handle) {
            bool ok = false;
            const QJsonObject answer = call([&](const Social::Callback &done) {
                Social::signUp(nick, handle, QStringLiteral("hunter22"), done);
            }, &ok);
            return ok ? answer : QJsonObject();
        };

        const QJsonObject ownerAcc = join(QStringLiteral("Role Owner"), QStringLiteral("roleowner"));
        QVERIFY2(!ownerAcc.isEmpty(), "the map owner must register");
        const QString ownerToken = ownerAcc.value(QStringLiteral("token")).toString();
        const QString ownerId = ownerAcc.value(QStringLiteral("id")).toString();

        const QJsonObject mateAcc = join(QStringLiteral("Role Mate"), QStringLiteral("rolemate"));
        QVERIFY2(!mateAcc.isEmpty(), "the co-owner must register");
        const QString mateToken = mateAcc.value(QStringLiteral("token")).toString();
        const QString mateId = mateAcc.value(QStringLiteral("id")).toString();

        const QJsonObject guestAcc = join(QStringLiteral("Role Guest"), QStringLiteral("roleguest"));
        QVERIFY2(!guestAcc.isEmpty(), "the tester must register");
        const QString guestToken = guestAcc.value(QStringLiteral("token")).toString();
        const QString guestId = guestAcc.value(QStringLiteral("id")).toString();

        // Пустой мир: список отдаётся, а owner ещё не заведён - его заводит
        // первый позвавший, и без этого приглашение не за что было бы принять.
        const RawReply empty = httpRequest(QStringLiteral("GET"),
                                           QStringLiteral("/api/worlds/101/members"),
                                           QByteArray(), ownerToken);
        QCOMPARE(empty.status, 200);
        QCOMPARE(empty.body.value(QStringLiteral("members")).toArray().size(), 0);
        QVERIFY(empty.body.value(QStringLiteral("owner")).isNull());

        // Без токена не работает ничего.
        QCOMPARE(httpRequest(QStringLiteral("GET"),
                             QStringLiteral("/api/worlds/101/members")).status, 401);
        QCOMPARE(httpPost(QStringLiteral("/api/worlds/101/members"),
                          {{QStringLiteral("player"), mateId}}).status, 401);

        // Первый, кто позвал, сам становится owner: лаунчер шлёт запрос с
        // машины хоста, то есть со стороны человека, которому карта принадлежит.
        const RawReply inviteMate = httpPost(QStringLiteral("/api/worlds/101/members"),
                                             {{QStringLiteral("player"), mateId},
                                              {QStringLiteral("role"), QStringLiteral("co-owner")}},
                                             ownerToken);
        QCOMPARE(inviteMate.status, 200);

        const RawReply ownerList = httpRequest(QStringLiteral("GET"),
                                               QStringLiteral("/api/worlds/101/members"),
                                               QByteArray(), ownerToken);
        QCOMPARE(ownerList.status, 200);
        QCOMPARE(ownerList.body.value(QStringLiteral("owner"))
                     .toObject().value(QStringLiteral("id")).toString(), ownerId);
        QCOMPARE(ownerList.body.value(QStringLiteral("myRole")).toString(),
                 QStringLiteral("owner"));
        QCOMPARE(ownerList.body.value(QStringLiteral("members")).toArray().size(), 2);
        QVERIFY(ownerList.body.value(QStringLiteral("members")).toArray().at(0)
                    .toObject().value(QStringLiteral("self")).toBool()
                || ownerList.body.value(QStringLiteral("members")).toArray().at(1)
                    .toObject().value(QStringLiteral("self")).toBool());

        // co-owner приглашает сам - ради этого роль и существует.
        const RawReply inviteGuest = httpPost(QStringLiteral("/api/worlds/101/members"),
                                              {{QStringLiteral("player"), guestId},
                                               {QStringLiteral("role"), QStringLiteral("tester")}},
                                              mateToken);
        QCOMPARE(inviteGuest.status, 200);

        // Ниже стоящий приглашать не может, иначе member сам себя повысит.
        const RawReply byTester = httpPost(QStringLiteral("/api/worlds/101/members"),
                                           {{QStringLiteral("player"), ownerId},
                                            {QStringLiteral("role"), QStringLiteral("member")}},
                                           guestToken);
        QCOMPARE(byTester.status, 403);
        QCOMPARE(byTester.body.value(QStringLiteral("error")).toString(),
                 QStringLiteral("forbidden"));

        // Валидация входа, а не прав.
        const RawReply badRole = httpPost(QStringLiteral("/api/worlds/101/members"),
                                          {{QStringLiteral("player"), guestId},
                                           {QStringLiteral("role"), QStringLiteral("admin")}},
                                          ownerToken);
        QCOMPARE(badRole.status, 400);
        QCOMPARE(badRole.body.value(QStringLiteral("error")).toString(),
                 QStringLiteral("bad_role"));

        const RawReply unknownTarget = httpPost(QStringLiteral("/api/worlds/101/members"),
                                                {{QStringLiteral("player"),
                                                  QStringLiteral("nobody-here")}},
                                                ownerToken);
        QCOMPARE(unknownTarget.status, 404);
        QCOMPARE(unknownTarget.body.value(QStringLiteral("error")).toString(),
                 QStringLiteral("unknown_player"));

        const RawReply selfInvite = httpPost(QStringLiteral("/api/worlds/101/members"),
                                             {{QStringLiteral("player"), ownerId}},
                                             ownerToken);
        QCOMPARE(selfInvite.status, 400);

        // Права на удаление.
        const RawReply dropOwner = httpRequest(QStringLiteral("DELETE"),
                                               QStringLiteral("/api/worlds/101/members/") + ownerId,
                                               QByteArray(), mateToken);
        QCOMPARE(dropOwner.status, 403);
        QCOMPARE(dropOwner.body.value(QStringLiteral("error")).toString(),
                 QStringLiteral("owner_stays"));

        const RawReply dropByTester = httpRequest(QStringLiteral("DELETE"),
                                                  QStringLiteral("/api/worlds/101/members/") + mateId,
                                                  QByteArray(), guestToken);
        QCOMPARE(dropByTester.status, 403);

        const RawReply dropMissing = httpRequest(QStringLiteral("DELETE"),
                                                 QStringLiteral("/api/worlds/101/members/nobody-here"),
                                                 QByteArray(), ownerToken);
        QCOMPARE(dropMissing.status, 404);
        QCOMPARE(dropMissing.body.value(QStringLiteral("error")).toString(),
                 QStringLiteral("not_a_member"));

        const RawReply dropByMate = httpRequest(QStringLiteral("DELETE"),
                                                QStringLiteral("/api/worlds/101/members/") + guestId,
                                                QByteArray(), mateToken);
        QCOMPARE(dropByMate.status, 200);

        // Повторное приглашение меняет роль, а не плодит вторую запись.
        const RawReply demote = httpPost(QStringLiteral("/api/worlds/101/members"),
                                         {{QStringLiteral("player"), mateId},
                                          {QStringLiteral("role"), QStringLiteral("member")}},
                                         ownerToken);
        QCOMPARE(demote.status, 200);
        const RawReply afterDemote = httpRequest(QStringLiteral("GET"),
                                                 QStringLiteral("/api/worlds/101/members"),
                                                 QByteArray(), ownerToken);
        QCOMPARE(afterDemote.body.value(QStringLiteral("members")).toArray().size(), 2);
        QCOMPARE(afterDemote.body.value(QStringLiteral("myRole")).toString(),
                 QStringLiteral("owner"));
        for (const QJsonValue &value : afterDemote.body.value(QStringLiteral("members")).toArray()) {
            const QJsonObject row = value.toObject();
            if (row.value(QStringLiteral("player")).toString() != mateId) continue;
            QCOMPARE(row.value(QStringLiteral("role")).toString(), QStringLiteral("member"));
        }

        // Выйти может каждый, кроме owner - он и есть автор карты.
        QCOMPARE(httpPost(QStringLiteral("/api/worlds/101/members"),
                          {{QStringLiteral("player"), guestId},
                           {QStringLiteral("role"), QStringLiteral("member")}},
                          ownerToken).status, 200);
        const RawReply leave = httpRequest(QStringLiteral("DELETE"),
                                           QStringLiteral("/api/worlds/101/members/") + guestId,
                                           QByteArray(), guestToken);
        QCOMPARE(leave.status, 200);
        const RawReply leaveOwner = httpRequest(QStringLiteral("DELETE"),
                                                QStringLiteral("/api/worlds/101/members/") + ownerId,
                                                QByteArray(), ownerToken);
        QCOMPARE(leaveOwner.status, 403);

        // co-owner равен co-owner по правам: снимать такого может только автор.
        // Оба должны быть co-owner, иначе проверка отваливается раньше - на
        // отсутствии прав у вызывающего.
        QCOMPARE(httpPost(QStringLiteral("/api/worlds/101/members"),
                          {{QStringLiteral("player"), guestId},
                           {QStringLiteral("role"), QStringLiteral("co-owner")}},
                          ownerToken).status, 200);
        QCOMPARE(httpPost(QStringLiteral("/api/worlds/101/members"),
                          {{QStringLiteral("player"), mateId},
                           {QStringLiteral("role"), QStringLiteral("co-owner")}},
                          ownerToken).status, 200);
        const RawReply peerDrop = httpRequest(QStringLiteral("DELETE"),
                                              QStringLiteral("/api/worlds/101/members/") + mateId,
                                              QByteArray(), guestToken);
        QCOMPARE(peerDrop.status, 403);
        QCOMPARE(peerDrop.body.value(QStringLiteral("error")).toString(),
                 QStringLiteral("needs_owner"));
        const RawReply ownerDrop = httpRequest(QStringLiteral("DELETE"),
                                               QStringLiteral("/api/worlds/101/members/") + mateId,
                                               QByteArray(), ownerToken);
        QCOMPARE(ownerDrop.status, 200);

        // Мир - только номер: мусорный id вообще не должен быть маршрутом.
        const RawReply junkWorld = httpRequest(QStringLiteral("GET"),
                                               QStringLiteral("/api/worlds/notanumber/members"),
                                               QByteArray(), ownerToken);
        QCOMPARE(junkWorld.status, 404);
    }

    void clientManagesWorldRoles()
    {
        // Те же маршруты, но через клиент social.cpp: именно ими пользуется
        // диалог соавторов, поэтому проверяем их, а не голый HTTP.
        auto join = [](const QString &nick, const QString &handle) {
            bool ok = false;
            const QJsonObject answer = call([&](const Social::Callback &done) {
                Social::signUp(nick, handle, QStringLiteral("hunter22"), done);
            }, &ok);
            return ok ? answer : QJsonObject();
        };

        const QJsonObject self = join(QStringLiteral("Role Caller"), QStringLiteral("rolecaller"));
        QVERIFY2(!self.isEmpty(), "the map owner must register");
        Settings::setSocialToken(self.value(QStringLiteral("token")).toString());

        const QJsonObject mate = join(QStringLiteral("Role Workmate"), QStringLiteral("roleworkmate"));
        QVERIFY2(!mate.isEmpty(), "the invited player must register");
        const QString mateId = mate.value(QStringLiteral("id")).toString();

        // Пустой мир отдаётся пустым, а не ошибкой.
        bool ok = false;
        QJsonObject body = call([](const Social::Callback &done) {
            Social::worldMembers(202, done);
        }, &ok);
        QVERIFY2(ok, "listing an empty world must succeed");
        QVERIFY(Social::memberListOf(body).isEmpty());

        // Первое приглашение делает нас владельцем и добавляет друга.
        ok = false;
        call([&](const Social::Callback &done) {
            Social::inviteToWorld(202, mateId, QStringLiteral("tester"), done);
        }, &ok);
        QVERIFY2(ok, "the first invitation must make me the owner");

        ok = false;
        body = call([](const Social::Callback &done) {
            Social::worldMembers(202, done);
        }, &ok);
        QVERIFY(ok);
        QCOMPARE(body.value(QStringLiteral("myRole")).toString(), QString("owner"));
        const QList<Social::Member> members = Social::memberListOf(body);
        QCOMPARE(members.size(), 2);
        bool foundMate = false;
        for (const Social::Member &member : members) {
            if (member.player != mateId) continue;
            foundMate = true;
            QCOMPARE(member.role, QString("tester"));
            QCOMPARE(member.handle, QString("roleworkmate"));
            QVERIFY(!member.self);
        }
        QVERIFY2(foundMate, "the invited friend must be in the list");

        // Снятие роли: друг уходит, остаюсь только я.
        ok = false;
        call([&](const Social::Callback &done) {
            Social::removeWorldMember(202, mateId, done);
        }, &ok);
        QVERIFY2(ok, "the owner must be able to remove a member");

        ok = false;
        body = call([](const Social::Callback &done) { Social::worldMembers(202, done); }, &ok);
        QVERIFY(ok);
        const QList<Social::Member> left = Social::memberListOf(body);
        QCOMPARE(left.size(), 1);
        QVERIFY(left.first().self);

        // Чужого игрока пригласить нельзя - сервер отвечает отказом.
        ok = true;
        call([&](const Social::Callback &done) {
            Social::inviteToWorld(202, QStringLiteral("nobody-here"), QStringLiteral("member"), done);
        }, &ok);
        QVERIFY2(!ok, "an unknown player must be refused");

        Settings::setSocialToken(QString());
    }

    void tokenBelongsToItsServer()
    {
        // Токен встроенного сервера не должен уходить на общий: там он
        // неизвестен, и лаунчер считал бы игрока вошедшим при сплошных 401.
        const QString local = Settings::socialUrl();
        Settings::setSocialToken(QStringLiteral("tok-local"));

        Settings::setSocialUrl(QStringLiteral("https://social.example.org"));
        QVERIFY2(Settings::socialToken().isEmpty(), "another server must not get the local token");
        Settings::setSocialToken(QStringLiteral("tok-shared"));

        // Встроенный сервер берёт новый порт на каждом запуске, а база одна -
        // вход на нём переживает смену порта.
        Settings::setSocialUrl(QStringLiteral("http://127.0.0.1:1"));
        QCOMPARE(Settings::socialToken(), QStringLiteral("tok-local"));

        Settings::setSocialUrl(QStringLiteral("https://social.example.org/"));
        QCOMPARE(Settings::socialToken(), QStringLiteral("tok-shared"));

        // Мёртвый адрес из 1.0.3 сбрасывается, а не держит лаунчер на себе.
        Settings::setSocialUrl(QStringLiteral("https://kogamaz.duckdns.org/social"));
        QVERIFY(Settings::socialUrl().isEmpty());

        Settings::setSocialUrl(local);
        Settings::setSocialToken(QString());
    }

    void accountKeepsPassword()
    {
        // Данные переживают перезапуск сервера: аккаунт читается из файла
        Settings::setSocialUrl(QString());
        m_server->stop();

        LocalSocialServer restarted;
        const QString url = restarted.start();
        QVERIFY(!url.isEmpty());
        Settings::setSocialUrl(url);

        bool ok = false;
        const QJsonObject body = call([&](const Social::Callback &done) {
            Social::login("feelz", "hunter22", done);
        }, &ok);
        QVERIFY2(ok, "the account must survive a launcher restart");
        QCOMPARE(body.value("nick").toString(), QString("FeelZ Dev"));

        restarted.stop();
        Settings::setSocialUrl(QString());
    }

private:
    static QString url() { return Settings::socialUrl(); }

    QTemporaryDir *m_data = nullptr;
    LocalSocialServer *m_server = nullptr;
    QString m_url;
    QString m_aliceId, m_aliceToken, m_bobId, m_bobToken;
};

QTEST_MAIN(LocalSocialTest)
#include "localsocial_test.moc"
