#include "localsocialserver.h"
#include "paths.h"

#include <algorithm>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QObject>
#include <QPasswordDigestor>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>

namespace {

// Пароли храним только хэшем: PBKDF2-SHA256 с уникальной солью. Повторяет
// подход Node-сервера, отличается лишь меньшим числом итераций - локально
// база маленькая, и ожидание ввода пароля не должно быть заметным.
constexpr int Pbkdf2Iterations = 20000;
constexpr int SaltBytes = 16;

// Ограничения совпадают с Node-сервером, чтобы клиент не зависел от того,
// какой именно сервер отвечает.
constexpr int MaxMessageLen = 1000;
constexpr int MaxPostLen = 500;
constexpr int DefaultPage = 50;
constexpr qint64 OnlineWindowMs = 90000;   // офлайн без heartbeat дольше 90 с

QByteArray randomBytes(int count)
{
    QByteArray out(count, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(out.data()),
                                          (count + 3) / 4);
    return out.left(count);
}

QString newId()
{
    return QString::fromLatin1(randomBytes(16).toHex());
}

// Соль и хэш храним по отдельности, восстановить пароль из них нельзя.
QString hashPassword(const QString &password, const QString &saltHex)
{
    const QByteArray salt = QByteArray::fromHex(saltHex.toLatin1());
    return QString::fromLatin1(QPasswordDigestor::deriveKeyPbkdf2(
        QCryptographicHash::Sha256, password.toUtf8(), salt,
        Pbkdf2Iterations, 32).toHex());
}

QString makeSalt()
{
    return QString::fromLatin1(randomBytes(SaltBytes).toHex());
}

bool verifyPassword(const QString &password, const QString &saltHex, const QString &hashHex)
{
    if (saltHex.isEmpty() || hashHex.isEmpty()) return false;
    return hashPassword(password, saltHex).compare(hashHex, Qt::CaseInsensitive) == 0;
}

// Уровень как в KoGaMa: на каждый уровень нужно больше опыта, чем на
// предыдущий, поэтому формула квадратичная. 0 опыта - всегда 1 уровень.
int levelFor(qint64 exp)
{
    if (exp <= 0) return 1;
    int level = 1;
    while (exp >= static_cast<qint64>(level) * level * 100) ++level;
    return level;
}

QString normalizeHandle(QString handle)
{
    handle = handle.trimmed().toLower();
    if (handle.startsWith(QLatin1Char('@'))) handle.remove(0, 1);
    return handle;
}

QString normalizeNick(const QString &nick)
{
    return nick.trimmed().toLower();
}

QString cleanText(const QString &text, int limit)
{
    QString out = text;
    out.replace(QLatin1Char('\r'), QString());
    out = out.trimmed();
    if (out.size() > limit) out = out.left(limit);
    return out;
}

} // namespace

// Хранилище: один JSON-файл со всеми таблицами. Держим всё в памяти и пишем
// целиком после изменения - локальная база на несколько аккаунтов, поэтому
// проще и надежнее, чем накатывать миграции на каждую правку.
class LocalSocialStore
{
public:
    QJsonObject root;          // accounts, friendships, requests, messages, posts, tokens
    QString path;
    bool dirty = false;

    bool load(const QString &file)
    {
        path = file;
        QFile f(path);
        if (!f.exists()) return true;
        if (!f.open(QIODevice::ReadOnly)) return false;
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        root = doc.isObject() ? doc.object() : QJsonObject();
        return true;
    }

    bool save()
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        f.close();
        dirty = false;
        return true;
    }

    void touch()
    {
        dirty = true;
        save();
    }

    // --- accounts ---
    QJsonObject accountById(const QString &id) const
    {
        return root.value(QLatin1String("accounts")).toObject().value(id).toObject();
    }

    QJsonObject accountByHandle(const QString &handle) const
    {
        const QJsonObject all = root.value(QLatin1String("accounts")).toObject();
        for (auto it = all.begin(); it != all.end(); ++it)
            if (it.value().toObject().value(QLatin1String("handle")).toString() == handle)
                return it.value().toObject();
        return QJsonObject();
    }

    QJsonObject accountByNick(const QString &nick) const
    {
        const QJsonObject all = root.value(QLatin1String("accounts")).toObject();
        for (auto it = all.begin(); it != all.end(); ++it)
            if (normalizeNick(it.value().toObject().value(QLatin1String("nick")).toString()) == nick)
                return it.value().toObject();
        return QJsonObject();
    }

    void putAccount(const QJsonObject &account)
    {
        QJsonObject all = root.value(QLatin1String("accounts")).toObject();
        all.insert(account.value(QLatin1String("id")).toString(), account);
        root.insert(QLatin1String("accounts"), all);
        touch();
    }

    // --- auth ---
    QString accountForToken(const QString &token) const
    {
        return root.value(QLatin1String("tokens")).toObject().value(token).toString();
    }

    void storeToken(const QString &token, const QString &id)
    {
        QJsonObject tokens = root.value(QLatin1String("tokens")).toObject();
        tokens.insert(token, id);
        root.insert(QLatin1String("tokens"), tokens);
        touch();
    }

    void dropToken(const QString &token)
    {
        QJsonObject tokens = root.value(QLatin1String("tokens")).toObject();
        if (!tokens.contains(token)) return;
        tokens.remove(token);
        root.insert(QLatin1String("tokens"), tokens);
        touch();
    }

    // --- friendships ---
    bool areFriends(const QString &a, const QString &b) const
    {
        const QJsonObject links = root.value(QLatin1String("friendships")).toObject();
        return links.value(a).toObject().contains(b) && links.value(b).toObject().contains(a);
    }

    void makeFriends(const QString &a, const QString &b)
    {
        QJsonObject links = root.value(QLatin1String("friendships")).toObject();
        QJsonObject ab = links.value(a).toObject();
        QJsonObject ba = links.value(b).toObject();
        ab.insert(b, true);
        ba.insert(a, true);
        links.insert(a, ab);
        links.insert(b, ba);
        root.insert(QLatin1String("friendships"), links);
        touch();
    }

    QStringList friendsOf(const QString &id) const
    {
        const QJsonObject links = root.value(QLatin1String("friendships")).toObject();
        const QJsonObject mine = links.value(id).toObject();
        return mine.keys();
    }

    // --- friend requests ---
    // Плоский список заявок {id, sender, target, created}: одна и та же запись
    // должна попадать и в incoming у цели, и в outgoing у отправителя, а с
    // отдельными списками на каждого пришлось бы синхронизировать две копии.
    QJsonArray requests() const
    {
        return root.value(QLatin1String("requests")).toArray();
    }

    void setRequests(const QJsonArray &list)
    {
        root.insert(QLatin1String("requests"), list);
        touch();
    }

    // --- presence ---
    QJsonObject presenceOf(const QString &id) const
    {
        return root.value(QLatin1String("presence")).toObject().value(id).toObject();
    }

    void setPresence(const QString &id, const QJsonObject &state)
    {
        QJsonObject all = root.value(QLatin1String("presence")).toObject();
        all.insert(id, state);
        root.insert(QLatin1String("presence"), all);
        touch();
    }

    bool isOnline(const QString &id) const
    {
        const qint64 seen = presenceOf(id).value(QLatin1String("updated")).toDouble();
        return seen > 0 && (QDateTime::currentMSecsSinceEpoch() - seen) < OnlineWindowMs;
    }

    // --- messages ---
    QJsonArray messagesBetween(const QString &a, const QString &b) const
    {
        const QString key = threadKey(a, b);
        return root.value(QLatin1String("messages")).toObject().value(key).toArray();
    }

    void appendMessage(const QString &a, const QString &b, const QJsonObject &msg)
    {
        QJsonObject all = root.value(QLatin1String("messages")).toObject();
        const QString key = threadKey(a, b);
        QJsonArray list = all.value(key).toArray();
        list.append(msg);
        all.insert(key, list);
        root.insert(QLatin1String("messages"), all);
        touch();
    }

    void setThread(const QString &a, const QString &b, const QJsonArray &list)
    {
        QJsonObject all = root.value(QLatin1String("messages")).toObject();
        all.insert(threadKey(a, b), list);
        root.insert(QLatin1String("messages"), all);
        touch();
    }

    // --- posts ---
    QJsonArray postsOf(const QString &id) const
    {
        return root.value(QLatin1String("posts")).toObject().value(id).toArray();
    }

    void setPostsOf(const QString &id, const QJsonArray &list)
    {
        QJsonObject all = root.value(QLatin1String("posts")).toObject();
        all.insert(id, list);
        root.insert(QLatin1String("posts"), all);
        touch();
    }

    // --- world members (Этап 3) ---
    // Роли соавторов одной карты. Мир - номер из игрового сервера, поэтому
    // храним объект по номеру мира, а внутри - плоский список {player, role,
    // created}. owner не выделяем в отдельное поле: автор карты и есть запись
    // с ролью owner, и менять её нельзя.
    QJsonArray membersOfWorld(const QString &world) const
    {
        return root.value(QLatin1String("world_members")).toObject().value(world).toArray();
    }

    void setMembersOfWorld(const QString &world, const QJsonArray &list)
    {
        QJsonObject all = root.value(QLatin1String("world_members")).toObject();
        all.insert(world, list);
        root.insert(QLatin1String("world_members"), all);
        touch();
    }

private:
    static QString threadKey(const QString &a, const QString &b)
    {
        QStringList parts{a, b};
        parts.sort();
        return parts.first() + QLatin1Char('|') + parts.last();
    }
};

LocalSocialServer::LocalSocialServer(QObject *parent)
    : QObject(parent)
    , m_server(new QTcpServer(this))
    , m_store(new LocalSocialStore())
{
    connect(m_server, &QTcpServer::newConnection, this, [this]() {
        while (m_server->hasPendingConnections()) handleConnection(m_server->nextPendingConnection());
    });}

LocalSocialServer::~LocalSocialServer()
{
    stop();
    delete m_store;
}

bool LocalSocialServer::isRunning() const
{
    return m_server && m_server->isListening();
}

quint16 LocalSocialServer::port() const
{
    return m_server ? m_server->serverPort() : 0;
}

void LocalSocialServer::stop()
{
    if (m_server) m_server->close();
    if (m_store && m_store->dirty) m_store->save();
}

QString LocalSocialServer::start()
{
    // Порт не фиксируем: если занят другим копом лаунчера, берём свободный.
    // Клиент узнаёт адрес из возвращаемой строки, а не считает его сами.
    if (!m_server->listen(QHostAddress::LocalHost, 0)) {
        emit failed(tr("Could not start the built-in social server: %1").arg(m_server->errorString()));
        return QString();
    }

    if (!m_store->load(Paths::data() + QLatin1String("/social.json")))
        emit failed(tr("Could not read the social data file. Accounts will not be saved."));

    return QStringLiteral("http://127.0.0.1:%1").arg(m_server->serverPort());
}

void LocalSocialServer::handleConnection(QTcpSocket *socket)
{
    socket->setParent(this);

    // Разбор в два прохода: сначала ждём полную шапку и вытаскиваем из неё
    // строку запроса, путь и токен, потом дожидаемся тела по Content-Length.
    // Шапку из буфера убираем сразу, иначе эти поля пришлось бы искать в теле.
    auto *buffer = new QByteArray;
    auto *headDone = new bool(false);
    auto *method = new QString;
    auto *path = new QString;
    auto *query = new QUrlQuery;
    auto *token = new QByteArray;
    auto *expected = new qint64(0);

    connect(socket, &QTcpSocket::readyRead, this,
            [this, socket, buffer, headDone, method, path, query, token, expected]() {
        buffer->append(socket->readAll());

        if (!*headDone) {
            const int split = buffer->indexOf("\r\n\r\n");
            if (split < 0) return;                    // шапка ещё не полная
            const QList<QByteArray> lines = buffer->left(split).split('\n');
            const QList<QByteArray> parts = lines.first().simplified().split(' ');
            if (parts.size() < 2) { socket->close(); return; }

            *method = QString::fromLatin1(parts.at(0));
            const QUrl url(QString::fromLatin1(parts.at(1)));
            *path = url.path();
            *query = QUrlQuery(url.query());

            for (const QByteArray &line : lines) {
                const QByteArray lower = line.trimmed().toLower();
                if (lower.startsWith("content-length:"))
                    *expected = lower.mid(15).trimmed().toLongLong();
                else if (lower.startsWith("authorization:")) {
                    const QByteArray value = line.trimmed().mid(14).trimmed();
                    if (value.startsWith("Bearer ")) *token = value.mid(7);
                }
            }
            *headDone = true;
            buffer->remove(0, split + 4);
        }

        if (buffer->size() < *expected) return;       // ждём тело
        route(*method, *path, *query, buffer->left(*expected), *token, socket);
    });

    connect(socket, &QTcpSocket::disconnected, this,
            [socket, buffer, headDone, method, path, query, token, expected]() {
        socket->deleteLater();
        delete buffer; delete headDone; delete method;
        delete path; delete query; delete token; delete expected;
    });
}

void LocalSocialServer::respond(QTcpSocket *socket, int status, const QByteArray &json)
{
    if (!socket) return;
    const char *reason = status == 200 ? "OK"
        : status == 201 ? "Created"
        : status == 400 ? "Bad Request"
        : status == 401 ? "Unauthorized"
        : status == 403 ? "Forbidden"
        : status == 404 ? "Not Found"
        : status == 409 ? "Conflict"
        : "Error";

    socket->write("HTTP/1.1 " + QByteArray::number(status) + " " + reason
        + "\r\nContent-Type: application/json; charset=utf-8"
        + "\r\nContent-Length: " + QByteArray::number(json.size())
        + "\r\nConnection: close\r\n\r\n" + json);
    socket->flush();
    socket->disconnectFromHost();
}

namespace {

// Ответ в формате, который ждёт клиент: коды и ключи ошибок совпадают с
// Node-сервером, чтобы social.cpp не различал два источника.
void sendError(QTcpSocket *socket, int status, const char *code, const QString &message)
{
    QJsonObject body{{QStringLiteral("error"), QLatin1String(code)},
                     {QStringLiteral("message"), message}};
    LocalSocialServer::respondJson(socket, status, body);
}

void sendOk(QTcpSocket *socket, const QJsonObject &body, int status = 200)
{
    LocalSocialServer::respondJson(socket, status, body);
}

// Аккаунт в том виде, в каком его отдаёт сервер клиенту.
QJsonObject publicAccount(const QJsonObject &account)
{
    QJsonObject out{{QStringLiteral("id"), account.value(QLatin1String("id")).toString()},
                    {QStringLiteral("nick"), account.value(QLatin1String("nick")).toString()},
                    {QStringLiteral("handle"), account.value(QLatin1String("handle")).toString()}};
    const qint64 exp = static_cast<qint64>(account.value(QLatin1String("exp")).toDouble());
    out.insert(QStringLiteral("exp"), static_cast<double>(exp));
    out.insert(QStringLiteral("level"), levelFor(exp));
    return out;
}

} // namespace

void LocalSocialServer::respondJson(QTcpSocket *socket, int status, const QJsonObject &body)
{
    respond(socket, status, QJsonDocument(body).toJson(QJsonDocument::Compact));
}

void LocalSocialServer::route(const QString &method, const QString &path,
                              const QUrlQuery &query, const QByteArray &body,
                              const QByteArray &token, QTcpSocket *socket)
{
    QJsonObject sent;
    if (!body.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (doc.isObject()) sent = doc.object();
    }

    // --- health -------------------------------------------------------------
    if (path == QLatin1String("/api/health")) {
        sendOk(socket, {{QStringLiteral("ok"), true},
                         {QStringLiteral("name"), QStringLiteral("kogamaz-local")},
                         {QStringLiteral("version"), QStringLiteral("1.1.0")},
                         {QStringLiteral("backend"), QStringLiteral("local")}});
        return;
    }

    // --- register -----------------------------------------------------------
    if (path == QLatin1String("/api/register") && method == QLatin1String("POST")) {
        const QString nick = cleanText(sent.value(QLatin1String("nick")).toString(), 32);
        const QString handle = normalizeHandle(sent.value(QLatin1String("handle")).toString());
        const QString password = sent.value(QLatin1String("password")).toString();

        if (nick.size() < 2) { sendError(socket, 400, "bad_nick", tr("Nickname is too short.")); return; }
        if (password.size() < 4) { sendError(socket, 400, "bad_password", tr("Password is too short.")); return; }
        static const QRegularExpression allowed(QStringLiteral("^[a-z0-9_]{3,20}$"));
        if (!allowed.match(handle).hasMatch()) {
            sendError(socket, 400, "bad_handle",
                      tr("Username may only contain letters, digits and underscores (3-20 characters)."));
            return;
        }
        if (!m_store->accountByNick(nick).isEmpty()) {
            sendError(socket, 409, "nick_taken", tr("That nickname is already taken.")); return;
        }
        if (!m_store->accountByHandle(handle).isEmpty()) {
            sendError(socket, 409, "handle_taken", tr("That @username is already taken.")); return;
        }

        const QString salt = makeSalt();
        QJsonObject account{{QStringLiteral("id"), newId()},
                            {QStringLiteral("nick"), nick},
                            {QStringLiteral("handle"), handle},
                            {QStringLiteral("salt"), salt},
                            {QStringLiteral("hash"), hashPassword(password, salt)},
                            {QStringLiteral("exp"), 0.0},
                            {QStringLiteral("created"),
                             static_cast<double>(QDateTime::currentMSecsSinceEpoch())}};
        m_store->putAccount(account);

        const QString session = newId();
        m_store->storeToken(session, account.value(QLatin1String("id")).toString());

        QJsonObject reply = publicAccount(account);
        reply.insert(QStringLiteral("token"), session);
        sendOk(socket, reply, 201);
        return;
    }

    // --- login --------------------------------------------------------------
    if (path == QLatin1String("/api/login") && method == QLatin1String("POST")) {
        QString who = sent.value(QLatin1String("nick")).toString().trimmed();
        const QString password = sent.value(QLatin1String("password")).toString();
        if (who.startsWith(QLatin1Char('@'))) who = who.mid(1);

        // Ищем и по нику, и по @username: игрок может ввести что угодно из двух,
        // гадать по префиксу незачем - коллизию решает проверка пароля.
        QJsonObject account = m_store->accountByNick(normalizeNick(who));
        if (account.isEmpty()) account = m_store->accountByHandle(normalizeHandle(who));

        if (account.isEmpty() || !verifyPassword(password,
                account.value(QLatin1String("salt")).toString(),
                account.value(QLatin1String("hash")).toString())) {
            sendError(socket, 401, "bad_credentials", tr("Wrong nickname or password."));
            return;
        }

        const QString session = newId();
        m_store->storeToken(session, account.value(QLatin1String("id")).toString());
        QJsonObject reply = publicAccount(account);
        reply.insert(QStringLiteral("token"), session);
        sendOk(socket, reply);
        return;
    }

    // --- me -----------------------------------------------------------------
    if (path == QLatin1String("/api/me")) {
        const QString id = m_store->accountForToken(QString::fromLatin1(token));
        if (id.isEmpty()) { sendError(socket, 401, "no_token", tr("Sign in first.")); return; }
        sendOk(socket, publicAccount(m_store->accountById(id)));
        return;
    }

    // --- logout -------------------------------------------------------------
    if (path == QLatin1String("/api/logout") && method == QLatin1String("POST")) {
        m_store->dropToken(QString::fromLatin1(token));
        sendOk(socket, {{QStringLiteral("ok"), true}});
        return;
    }

    routePeople(method, path, query, sent, token, socket);
}

// --- helpers for the people/chat/posts routes ------------------------------

namespace {

// id игрока из токена; пусто = не вошёл.
QString meOf(LocalSocialStore *store, const QByteArray &token)
{
    return store->accountForToken(QString::fromLatin1(token));
}

// Тот же набор состояний, что у relationOf в Node-сервере: по нему клиент
// решает, кнопку "Добавить" показывать или "Отменить заявку".
QString relationOf(LocalSocialStore *store, const QString &me, const QString &other)
{
    if (me == other) return QStringLiteral("self");
    if (store->areFriends(me, other)) return QStringLiteral("friend");
    const QJsonArray all = store->requests();
    for (const QJsonValue &value : all) {
        const QJsonObject row = value.toObject();
        const QString from = row.value(QLatin1String("sender")).toString();
        const QString to = row.value(QLatin1String("target")).toString();
        if (from == me && to == other) return QStringLiteral("outgoing");
        if (from == other && to == me) return QStringLiteral("incoming");
    }
    return QStringLiteral("none");
}

QJsonObject personOf(LocalSocialStore *store, const QString &id)
{
    const QJsonObject account = store->accountById(id);
    return {{QStringLiteral("id"), id},
            {QStringLiteral("nick"), account.value(QLatin1String("nick")).toString()},
            {QStringLiteral("handle"), account.value(QLatin1String("handle")).toString()}};
}

// --- роли соавторов карты (Этап 3) -----------------------------------------
//
// Тот же набор ролей и те же коды ошибок, что в social-server/server.js:
// клиент не должен замечать, отвечает встроенный сервер или общий.
//
// Роли - только учёт в лаунчере. Настоящий доступ к стройке решает игровой
// сервер общим joinMode на весь сервер, а закрывать его по-отдельности умеет
// лишь его плагин на .NET (SDK в проде нет), так что роль здесь - это
// «кто кого позвал и кто может звать дальше», а не запрет в игре.
const QStringList memberRoles{QStringLiteral("co-owner"),
                              QStringLiteral("tester"),
                              QStringLiteral("member")};

int roleRank(const QString &role)
{
    if (role == QLatin1String("owner")) return 3;
    if (role == QLatin1String("co-owner")) return 2;
    return 1;   // tester и member по правам не различаются
}

bool roleCanManage(const QString &role)
{
    return roleRank(role) >= 2;
}

} // namespace

void LocalSocialServer::routePeople(const QString &method, const QString &path,
                                    const QUrlQuery &query, const QJsonObject &sent,
                                    const QByteArray &token, QTcpSocket *socket)
{
    const QString me = meOf(m_store, token);

    // --- search -------------------------------------------------------------
    if (path == QLatin1String("/api/search") && method == QLatin1String("GET")) {
        if (me.isEmpty()) { sendError(socket, 401, "no_token", tr("Sign in first.")); return; }

        QString term = query.queryItemValue(QStringLiteral("nick")).trimmed().toLower();
        if (term.startsWith(QLatin1Char('@'))) term.remove(0, 1);
        if (term.isEmpty()) { sendOk(socket, {{QStringLiteral("results"), QJsonArray()}}); return; }

        // Порядок как в эталонном сервере: точный @username, потом префикс
        // по нему, потом совпадение по нику - так друг находится по @username.
        QStringList ids;
        const QJsonObject all = m_store->root.value(QLatin1String("accounts")).toObject();
        for (auto it = all.begin(); it != all.end(); ++it) {
            const QJsonObject account = it.value().toObject();
            const QString handle = account.value(QLatin1String("handle")).toString();
            if (handle == term) ids.prepend(it.key());
            else if (handle.startsWith(term)) ids.append(it.key());
        }
        for (auto it = all.begin(); it != all.end(); ++it) {
            const QJsonObject account = it.value().toObject();
            if (ids.contains(it.key())) continue;
            if (account.value(QLatin1String("nick")).toString().toLower().contains(term))
                ids.append(it.key());
        }

        QJsonArray results;
        for (const QString &id : ids.mid(0, 25)) {
            QJsonObject row = personOf(m_store, id);
            row.insert(QStringLiteral("relation"), relationOf(m_store, me, id));
            results.append(row);
        }
        sendOk(socket, {{QStringLiteral("results"), results}});
        return;
    }

    // --- friends ------------------------------------------------------------
    if (path == QLatin1String("/api/friends") && method == QLatin1String("GET")) {
        if (me.isEmpty()) { sendError(socket, 401, "no_token", tr("Sign in first.")); return; }

        QJsonArray rows;
        const QStringList ids = m_store->friendsOf(me);
        for (const QString &id : ids) {
            const QJsonObject state = m_store->presenceOf(id);
            QJsonObject row = personOf(m_store, id);
            row.insert(QStringLiteral("online"), m_store->isOnline(id));
            row.insert(QStringLiteral("world"), state.value(QLatin1String("world")).toString());
            row.insert(QStringLiteral("address"), state.value(QLatin1String("address")).toString());
            rows.append(row);
        }
        // сортируем по нику, чтобы порядок не прыгал от вызова к вызову;
        // QJsonArray не даёт итераторов для std::sort, поэтому идём через
        // обычный список и заново собираем массив
        QList<QJsonObject> sorted;
        for (const QJsonValue &value : rows) sorted.append(value.toObject());
        std::sort(sorted.begin(), sorted.end(), [](const QJsonObject &a, const QJsonObject &b) {
            return a.value(QLatin1String("nick")).toString()
                .localeAwareCompare(b.value(QLatin1String("nick")).toString()) < 0;
        });
        rows = QJsonArray();
        for (const QJsonObject &row : sorted) rows.append(row);
        sendOk(socket, {{QStringLiteral("friends"), rows}});
        return;
    }

    // --- friend requests ----------------------------------------------------
    if (path == QLatin1String("/api/requests") && method == QLatin1String("GET")) {
        if (me.isEmpty()) { sendError(socket, 401, "no_token", tr("Sign in first.")); return; }

        QJsonArray incoming, outgoing;
        const QJsonArray all = m_store->requests();
        for (const QJsonValue &value : all) {
            const QJsonObject row = value.toObject();
            const QString from = row.value(QLatin1String("sender")).toString();
            const QString to = row.value(QLatin1String("target")).toString();
            if (from != me && to != me) continue;

            QJsonObject item{{QStringLiteral("id"), row.value(QLatin1String("id")).toString()},
                             {QStringLiteral("player"), from == me ? to : from},
                             {QStringLiteral("created"), row.value(QLatin1String("created"))}};
            const QJsonObject account = m_store->accountById(from == me ? to : from);
            item.insert(QStringLiteral("nick"), account.value(QLatin1String("nick")).toString());
            item.insert(QStringLiteral("handle"), account.value(QLatin1String("handle")).toString());
            if (to == me) incoming.append(item); else outgoing.append(item);
        }
        sendOk(socket, {{QStringLiteral("incoming"), incoming},
                         {QStringLiteral("outgoing"), outgoing}});
        return;
    }

    if (path == QLatin1String("/api/requests") && method == QLatin1String("POST")) {
        if (me.isEmpty()) { sendError(socket, 401, "no_token", tr("Sign in first.")); return; }

        const QString target = sent.value(QLatin1String("targetId")).toString().trimmed();
        if (target.isEmpty()) { sendError(socket, 400, "bad_target", tr("Pick a player first.")); return; }
        if (target == me) { sendError(socket, 400, "bad_target", tr("You cannot add yourself.")); return; }
        if (m_store->accountById(target).isEmpty()) {
            sendError(socket, 404, "unknown_player", tr("That player does not exist."));
            return;
        }
        if (m_store->areFriends(me, target)) {
            sendError(socket, 409, "already_friends", tr("You are already friends."));
            return;
        }

        QJsonArray all = m_store->requests();
        for (const QJsonValue &value : all) {
            const QJsonObject row = value.toObject();
            const QString from = row.value(QLatin1String("sender")).toString();
            const QString to = row.value(QLatin1String("target")).toString();
            if ((from == me && to == target) || (from == target && to == me)) {
                sendError(socket, 409, "request_exists", tr("A friend request already exists."));
                return;
            }
        }

        all.append(QJsonObject{{QStringLiteral("id"), newId()},
                               {QStringLiteral("sender"), me},
                               {QStringLiteral("target"), target},
                               {QStringLiteral("created"),
                                static_cast<double>(QDateTime::currentMSecsSinceEpoch())}});
        m_store->setRequests(all);
        sendOk(socket, {{QStringLiteral("ok"), true}}, 201);
        return;
    }

    // accept / reject / cancel
    if (method == QLatin1String("POST") && path.startsWith(QLatin1String("/api/requests/"))) {
        if (me.isEmpty()) { sendError(socket, 401, "no_token", tr("Sign in first.")); return; }

        const QString rest = path.mid(QStringLiteral("/api/requests/").size());
        const int split = rest.indexOf(QLatin1Char('/'));
        if (split < 0) { sendError(socket, 404, "not_found", tr("Unknown endpoint.")); return; }
        const QString id = rest.left(split);
        const QString action = rest.mid(split + 1);

        QJsonArray all = m_store->requests();
        int index = -1;
        for (int i = 0; i < all.size(); ++i) {
            if (all.at(i).toObject().value(QLatin1String("id")).toString() == id) { index = i; break; }
        }

        if (action == QLatin1String("cancel")) {
            if (index >= 0 && all.at(index).toObject().value(QLatin1String("sender")).toString() == me)
                all.removeAt(index);
            m_store->setRequests(all);
            sendOk(socket, {{QStringLiteral("ok"), true}});
            return;
        }

        if (index < 0 || all.at(index).toObject().value(QLatin1String("target")).toString() != me) {
            sendError(socket, 404, "unknown_request", tr("That request no longer exists."));
            return;
        }
        if (action == QLatin1String("reject")) {
            all.removeAt(index);
            m_store->setRequests(all);
            sendOk(socket, {{QStringLiteral("ok"), true}});
            return;
        }
        if (action != QLatin1String("accept")) {
            sendError(socket, 404, "not_found", tr("Unknown endpoint."));
            return;
        }

        // Приняли - дружба, а обе встречные заявки (если успела появиться)
        // снимаются, иначе она осталась бы висеть после установления дружбы.
        const QString from = all.at(index).toObject().value(QLatin1String("sender")).toString();
        all.removeAt(index);
        QJsonArray kept;
        for (const QJsonValue &value : all) {
            const QJsonObject row = value.toObject();
            const QString s = row.value(QLatin1String("sender")).toString();
            const QString t = row.value(QLatin1String("target")).toString();
            if ((s == from && t == me) || (s == me && t == from)) continue;
            kept.append(row);
        }
        m_store->setRequests(kept);
        m_store->makeFriends(from, me);
        sendOk(socket, {{QStringLiteral("ok"), true}});
        return;
    }

    // --- presence -----------------------------------------------------------
    if (path == QLatin1String("/api/presence") && method == QLatin1String("POST")) {
        if (me.isEmpty()) { sendError(socket, 401, "no_token", tr("Sign in first.")); return; }

        const bool isPublic = sent.value(QLatin1String("public")).toBool();
        QJsonObject state{{QStringLiteral("world"), cleanText(sent.value(QLatin1String("world")).toString(), 80)},
                          {QStringLiteral("address"), cleanText(sent.value(QLatin1String("address")).toString(), 200)},
                          {QStringLiteral("public"), isPublic},
                          {QStringLiteral("updated"),
                           static_cast<double>(QDateTime::currentMSecsSinceEpoch())}};
        m_store->setPresence(me, state);
        sendOk(socket, {{QStringLiteral("ok"), true},
                         {QStringLiteral("online"), true},
                         {QStringLiteral("public"), isPublic}});
        return;
    }

    // --- hosting ------------------------------------------------------------
    if (path == QLatin1String("/api/hosting") && method == QLatin1String("GET")) {
        if (me.isEmpty()) { sendError(socket, 401, "no_token", tr("Sign in first.")); return; }

        // Публичный список хостящих: видят все вошедшие, приватные миры сюда
        // не попадают - они остаются только в списке друзей.
        QJsonArray hosting;
        const QJsonObject all = m_store->root.value(QLatin1String("presence")).toObject();
        for (auto it = all.begin(); it != all.end(); ++it) {
            const QJsonObject state = it.value().toObject();
            if (!state.value(QLatin1String("public")).toBool()) continue;
            const QString address = state.value(QLatin1String("address")).toString();
            if (address.isEmpty()) continue;
            const qint64 updated = static_cast<qint64>(state.value(QLatin1String("updated")).toDouble());
            if (updated <= 0 || QDateTime::currentMSecsSinceEpoch() - updated >= OnlineWindowMs) continue;

            const QJsonObject account = m_store->accountById(it.key());
            hosting.append(QJsonObject{{QStringLiteral("player"), it.key()},
                                       {QStringLiteral("nick"), account.value(QLatin1String("nick")).toString()},
                                       {QStringLiteral("handle"), account.value(QLatin1String("handle")).toString()},
                                       {QStringLiteral("title"), state.value(QLatin1String("world")).toString()},
                                       {QStringLiteral("address"), address}});
        }
        sendOk(socket, {{QStringLiteral("hosting"), hosting}});
        return;
    }

    routeChat(method, path, query, sent, token, socket);
}

void LocalSocialServer::routeChat(const QString &method, const QString &path,
                                  const QUrlQuery &query, const QJsonObject &sent,
                                  const QByteArray &token, QTcpSocket *socket)
{
    const QString me = meOf(m_store, token);

    // Переписка только между друзьями: без этой проверки личные сообщения
    // становятся каналом спама на незнакомых людей.
    const auto friendOnly = [this, socket, me](const QString &peer) {
        if (peer.isEmpty()) { sendError(socket, 400, "bad_peer", tr("Pick a player first.")); return false; }
        if (relationOf(m_store, me, peer) != QLatin1String("friend")) {
            sendError(socket, 403, "not_friends", tr("You can only message your friends."));
            return false;
        }
        return true;
    };

    // --- history ------------------------------------------------------------
    if (path == QLatin1String("/api/messages") && method == QLatin1String("GET")) {
        if (me.isEmpty()) { sendError(socket, 401, "no_token", tr("Sign in first.")); return; }
        const QString peer = query.queryItemValue(QStringLiteral("peer")).trimmed();
        if (!friendOnly(peer)) return;

        // `before` - курсор по created: страница уходит один раз, поэтому
        // сообщения, пришедшие пока листали, не дублируются.
        const qint64 before = query.queryItemValue(QStringLiteral("before")).toLongLong();
        int limit = query.queryItemValue(QStringLiteral("limit")).toInt();
        if (limit < 1) limit = DefaultPage;
        if (limit > DefaultPage) limit = DefaultPage;

        QJsonArray all = m_store->messagesBetween(me, peer);
        // назад-по-времени, как принято в мессенджерах, и режем по курсору
        QJsonArray page;
        int taken = 0;
        for (int i = all.size() - 1; i >= 0 && taken < limit; --i) {
            const QJsonObject row = all.at(i).toObject();
            const qint64 created = static_cast<qint64>(row.value(QLatin1String("created")).toDouble());
            if (before > 0 && created >= before) continue;
            page.prepend(row);
            ++taken;
        }

        QJsonArray messages;
        for (const QJsonValue &value : page) {
            const QJsonObject row = value.toObject();
            messages.append(QJsonObject{
                {QStringLiteral("id"), row.value(QLatin1String("id")).toString()},
                {QStringLiteral("mine"), row.value(QLatin1String("sender")).toString() == me},
                {QStringLiteral("body"), row.value(QLatin1String("body")).toString()},
                {QStringLiteral("created"), row.value(QLatin1String("created"))},
                {QStringLiteral("read"), row.value(QLatin1String("read")).toBool()}});
        }
        sendOk(socket, {{QStringLiteral("messages"), messages},
                         {QStringLiteral("hasMore"), taken == limit && all.size() > taken}});
        return;
    }

    // --- send ---------------------------------------------------------------
    if (path == QLatin1String("/api/messages") && method == QLatin1String("POST")) {
        if (me.isEmpty()) { sendError(socket, 401, "no_token", tr("Sign in first.")); return; }
        const QString peer = sent.value(QLatin1String("peer")).toString().trimmed();
        if (!friendOnly(peer)) return;

        const QString text = cleanText(sent.value(QLatin1String("body")).toString(), MaxMessageLen);
        if (text.isEmpty()) { sendError(socket, 400, "empty_message", tr("Message cannot be empty.")); return; }

        const qint64 created = QDateTime::currentMSecsSinceEpoch();
        QJsonObject row{{QStringLiteral("id"), newId()},
                        {QStringLiteral("sender"), me},
                        {QStringLiteral("recipient"), peer},
                        {QStringLiteral("body"), text},
                        {QStringLiteral("created"), static_cast<double>(created)},
                        {QStringLiteral("read"), false}};
        m_store->appendMessage(me, peer, row);
        sendOk(socket, {{QStringLiteral("ok"), true},
                         {QStringLiteral("id"), row.value(QStringLiteral("id"))},
                         {QStringLiteral("mine"), true},
                         {QStringLiteral("body"), text},
                         {QStringLiteral("created"), static_cast<double>(created)}}, 201);
        return;
    }

    // --- read marker --------------------------------------------------------
    if (path == QLatin1String("/api/messages/read") && method == QLatin1String("POST")) {
        if (me.isEmpty()) { sendError(socket, 401, "no_token", tr("Sign in first.")); return; }
        const QString peer = sent.value(QLatin1String("peer")).toString().trimmed();
        if (!friendOnly(peer)) return;

        // Прочитанным считаем то, что прислал собеседник: свой статус отвечает
        // уже за другую сторону и править его здесь незачем.
        QJsonArray all = m_store->messagesBetween(me, peer);
        bool changed = false;
        QJsonArray kept;
        for (const QJsonValue &value : all) {
            QJsonObject row = value.toObject();
            if (row.value(QLatin1String("sender")).toString() == peer
                && !row.value(QLatin1String("read")).toBool()) {
                row.insert(QStringLiteral("read"), true);
                changed = true;
            }
            kept.append(row);
        }
        if (changed) m_store->setThread(me, peer, kept);
        sendOk(socket, {{QStringLiteral("ok"), true}});
        return;
    }

    // --- unread counter -----------------------------------------------------
    if (path == QLatin1String("/api/unread") && method == QLatin1String("GET")) {
        if (me.isEmpty()) { sendError(socket, 401, "no_token", tr("Sign in first.")); return; }

        QJsonObject counts;   // собеседник -> сколько не прочитано
        const QJsonObject threads = m_store->root.value(QLatin1String("messages")).toObject();
        for (auto it = threads.begin(); it != threads.end(); ++it) {
            if (!it.key().contains(me)) continue;   // чужие треды нас не касаются
            for (const QJsonValue &value : it.value().toArray()) {
                const QJsonObject row = value.toObject();
                if (row.value(QLatin1String("recipient")).toString() != me) continue;
                if (row.value(QLatin1String("read")).toBool()) continue;
                const QString peer = row.value(QLatin1String("sender")).toString();
                counts.insert(peer, counts.value(peer).toInt() + 1);
            }
        }

        QJsonArray unread;
        for (auto it = counts.begin(); it != counts.end(); ++it)
            unread.append(QJsonObject{{QStringLiteral("player"), it.key()},
                                      {QStringLiteral("count"), it.value().toInt()}});
        sendOk(socket, {{QStringLiteral("unread"), unread}});
        return;
    }

    routePosts(method, path, query, sent, token, socket);
}

void LocalSocialServer::routePosts(const QString &method, const QString &path,
                                   const QUrlQuery &query, const QJsonObject &sent,
                                   const QByteArray &token, QTcpSocket *socket)
{
    const QString me = meOf(m_store, token);
    if (me.isEmpty()) { sendError(socket, 401, "no_token", tr("Sign in first.")); return; }

    // --- feed ---------------------------------------------------------------
    if (path == QLatin1String("/api/posts") && method == QLatin1String("GET")) {
        // Посты профиля видят сам автор и его друзья - профиль открывается
        // по @username, но содержимое остаётся для своих.
        QString author = query.queryItemValue(QStringLiteral("player")).trimmed();
        if (author.isEmpty()) author = me;
        if (author != me && relationOf(m_store, me, author) != QLatin1String("friend")) {
            sendError(socket, 403, "not_friends", tr("Posts are visible to friends only."));
            return;
        }

        const qint64 before = query.queryItemValue(QStringLiteral("before")).toLongLong();
        int limit = query.queryItemValue(QStringLiteral("limit")).toInt();
        if (limit < 1) limit = DefaultPage;
        if (limit > DefaultPage) limit = DefaultPage;

        QJsonArray all = m_store->postsOf(author);
        QJsonArray page;
        int taken = 0;
        for (int i = all.size() - 1; i >= 0 && taken < limit; --i) {
            const QJsonObject row = all.at(i).toObject();
            const qint64 created = static_cast<qint64>(row.value(QLatin1String("created")).toDouble());
            if (before > 0 && created >= before) continue;
            page.prepend(row);
            ++taken;
        }

        QJsonArray posts;
        for (const QJsonValue &value : page) {
            const QJsonObject row = value.toObject();
            posts.append(QJsonObject{
                {QStringLiteral("id"), row.value(QLatin1String("id")).toString()},
                {QStringLiteral("body"), row.value(QLatin1String("body")).toString()},
                {QStringLiteral("created"), row.value(QLatin1String("created"))}});
        }

        const QJsonObject owner = m_store->accountById(author);
        sendOk(socket, {{QStringLiteral("player"), author},
                         {QStringLiteral("nick"), owner.value(QLatin1String("nick")).toString()},
                         {QStringLiteral("handle"), owner.value(QLatin1String("handle")).toString()},
                         {QStringLiteral("posts"), posts},
                         {QStringLiteral("hasMore"), taken == limit && all.size() > taken}});
        return;
    }

    // --- new post -----------------------------------------------------------
    if (path == QLatin1String("/api/posts") && method == QLatin1String("POST")) {
        const QString text = cleanText(sent.value(QLatin1String("body")).toString(), MaxPostLen);
        if (text.isEmpty()) { sendError(socket, 400, "empty_post", tr("Post cannot be empty.")); return; }

        const qint64 created = QDateTime::currentMSecsSinceEpoch();
        QJsonObject row{{QStringLiteral("id"), newId()},
                        {QStringLiteral("author"), me},
                        {QStringLiteral("body"), text},
                        {QStringLiteral("created"), static_cast<double>(created)}};
        QJsonArray mine = m_store->postsOf(me);
        mine.append(row);
        m_store->setPostsOf(me, mine);

        // Опыт за активность: пост - заметное действие, поэтому даём больше,
        // чем за сообщение в чате.
        QJsonObject account = m_store->accountById(me);
        account.insert(QStringLiteral("exp"),
                       account.value(QLatin1String("exp")).toDouble() + 10.0);
        m_store->putAccount(account);

        sendOk(socket, {{QStringLiteral("ok"), true},
                         {QStringLiteral("id"), row.value(QStringLiteral("id"))},
                         {QStringLiteral("body"), text},
                         {QStringLiteral("created"), static_cast<double>(created)}}, 201);
        return;
    }

    // --- delete own post ----------------------------------------------------
    if (path.startsWith(QLatin1String("/api/posts/")) && method == QLatin1String("DELETE")) {
        const QString id = path.mid(QStringLiteral("/api/posts/").size());
        QJsonArray mine = m_store->postsOf(me);
        QJsonArray kept;
        for (const QJsonValue &value : mine) {
            if (value.toObject().value(QLatin1String("id")).toString() == id) continue;
            kept.append(value);
        }
        // Чужой пост не трогаем и всё равно отвечаем 200: удаление чужого
        // должно выглядеть успешным, иначе по id подделывают существование.
        if (kept.size() != mine.size()) m_store->setPostsOf(me, kept);
        sendOk(socket, {{QStringLiteral("ok"), true}});
        return;
    }

    // Остальное - роли соавторов, а дальше уже неизвестный путь.
    routeWorlds(method, path, query, sent, token, socket);
}

void LocalSocialServer::routeWorlds(const QString &method, const QString &path,
                                    const QUrlQuery &query, const QJsonObject &sent,
                                    const QByteArray &token, QTcpSocket *socket)
{
    Q_UNUSED(query)
    const QString me = meOf(m_store, token);

    // Мир - только номер: мусорный id не должен дойти до таблицы вообще,
    // поэтому цифры проверяем прямо в маске, как в Node-сервере.
    static const QRegularExpression listRe(
        QStringLiteral("^/api/worlds/(\\d+)/members$"));
    static const QRegularExpression dropRe(
        QStringLiteral("^/api/worlds/(\\d+)/members/([^/]+)$"));

    const QRegularExpressionMatch listMatch =
        (method == QLatin1String("GET") || method == QLatin1String("POST"))
        ? listRe.match(path) : QRegularExpressionMatch();
    const QRegularExpressionMatch dropMatch =
        method == QLatin1String("DELETE") ? dropRe.match(path) : QRegularExpressionMatch();

    // --- список соавторов ---------------------------------------------------
    if (method == QLatin1String("GET") && listMatch.hasMatch()) {
        if (me.isEmpty()) {
            sendError(socket, 401, "unauthorized", tr("Sign in to use social features.")); return;
        }
        const QString world = listMatch.captured(1);
        const QJsonArray rows = m_store->membersOfWorld(world);

        QString owner;
        QJsonArray members;
        QString myRole;
        for (const QJsonValue &value : rows) {
            const QJsonObject row = value.toObject();
            const QString player = row.value(QLatin1String("player")).toString();
            const QString role = row.value(QLatin1String("role")).toString();
            const QJsonObject account = m_store->accountById(player);
            if (role == QLatin1String("owner")) owner = player;
            if (player == me) myRole = role;
            members.append(QJsonObject{
                {QStringLiteral("player"), player},
                {QStringLiteral("nick"), account.value(QLatin1String("nick")).toString()},
                {QStringLiteral("handle"), account.value(QLatin1String("handle")).toString()},
                {QStringLiteral("role"), role},
                {QStringLiteral("created"), row.value(QLatin1String("created"))},
                {QStringLiteral("self"), player == me}});
        }

        sendOk(socket, {{QStringLiteral("world"), world},
                         {QStringLiteral("myRole"), myRole.isNull() ? QJsonValue(QJsonValue::Null)
                                                                    : QJsonValue(myRole)},
                         {QStringLiteral("owner"),
                          owner.isEmpty() ? QJsonValue(QJsonValue::Null)
                                          : QJsonValue(personOf(m_store, owner))},
                         {QStringLiteral("members"), members}});
        return;
    }

    // --- пригласить или сменить роль ----------------------------------------
    if (method == QLatin1String("POST") && listMatch.hasMatch()) {
        if (me.isEmpty()) {
            sendError(socket, 401, "unauthorized", tr("Sign in to use social features.")); return;
        }
        const QString world = listMatch.captured(1);
        const QString target = sent.value(QLatin1String("player")).toString().trimmed();
        if (target.isEmpty()) {
            sendError(socket, 400, "bad_player", tr("Pick a player to invite.")); return;
        }
        const QString role = sent.value(QLatin1String("role")).toString()
                                 .trimmed().toLower();
        const QString wanted = role.isEmpty() ? QStringLiteral("member") : role;
        if (!memberRoles.contains(wanted)) {
            sendError(socket, 400, "bad_role", tr("Role must be co-owner, tester or member."));
            return;
        }
        if (target == me) {
            sendError(socket, 400, "bad_player", tr("You already have a role on this map."));
            return;
        }

        QJsonArray rows = m_store->membersOfWorld(world);
        auto hasOwner = [&rows]() {
            for (const QJsonValue &value : rows)
                if (value.toObject().value(QLatin1String("role")).toString()
                    == QLatin1String("owner")) return true;
            return false;
        };
        auto roleOf = [&rows](const QString &player) {
            for (const QJsonValue &value : rows) {
                const QJsonObject row = value.toObject();
                if (row.value(QLatin1String("player")).toString() == player)
                    return row.value(QLatin1String("role")).toString();
            }
            return QString();
        };

        // Первый, кто позвал, сам становится owner: лаунчер шлёт запрос с
        // машины хоста, то есть со стороны человека, которому карта принадлежит.
        if (!hasOwner()) {
            rows.append(QJsonObject{{QStringLiteral("player"), me},
                                    {QStringLiteral("role"), QStringLiteral("owner")},
                                    {QStringLiteral("created"),
                                     static_cast<double>(QDateTime::currentMSecsSinceEpoch())}});
            m_store->setMembersOfWorld(world, rows);
        }

        if (!roleCanManage(roleOf(me))) {
            sendError(socket, 403, "forbidden", tr("Only the owner or a co-owner can invite."));
            return;
        }
        if (m_store->accountById(target).isEmpty()) {
            sendError(socket, 404, "unknown_player", tr("No such player.")); return;
        }

        // Повторное приглашение меняет роль, а не плодит вторую запись.
        const qint64 created = QDateTime::currentMSecsSinceEpoch();
        QJsonArray kept;
        bool replaced = false;
        for (const QJsonValue &value : rows) {
            const QJsonObject row = value.toObject();
            if (row.value(QLatin1String("player")).toString() == target) {
                QJsonObject changed = row;
                changed.insert(QStringLiteral("role"), wanted);
                kept.append(changed);
                replaced = true;
            } else {
                kept.append(row);
            }
        }
        if (!replaced)
            kept.append(QJsonObject{{QStringLiteral("player"), target},
                                    {QStringLiteral("role"), wanted},
                                    {QStringLiteral("created"), static_cast<double>(created)}});
        m_store->setMembersOfWorld(world, kept);

        sendOk(socket, {{QStringLiteral("ok"), true},
                         {QStringLiteral("world"), world},
                         {QStringLiteral("player"), target},
                         {QStringLiteral("role"), wanted}});
        return;
    }

    // --- снять роль ---------------------------------------------------------
    if (method == QLatin1String("DELETE") && dropMatch.hasMatch()) {
        if (me.isEmpty()) {
            sendError(socket, 401, "unauthorized", tr("Sign in to use social features.")); return;
        }
        const QString world = dropMatch.captured(1);
        const QString target = dropMatch.captured(2);
        const QJsonArray rows = m_store->membersOfWorld(world);

        QString mine, theirs;
        for (const QJsonValue &value : rows) {
            const QJsonObject row = value.toObject();
            const QString player = row.value(QLatin1String("player")).toString();
            const QString role = row.value(QLatin1String("role")).toString();
            if (player == me) mine = role;
            if (player == target) theirs = role;
        }

        if (theirs.isEmpty()) {
            sendError(socket, 404, "not_a_member", tr("That player is not on this map.")); return;
        }
        if (theirs == QLatin1String("owner")) {
            sendError(socket, 403, "owner_stays", tr("The owner cannot be removed.")); return;
        }
        // Выйти может каждый, кроме owner - он и есть автор карты.
        if (target != me) {
            if (!roleCanManage(mine)) {
                sendError(socket, 403, "forbidden",
                          tr("Only the owner or a co-owner can remove.")); return;
            }
            if (theirs == QLatin1String("co-owner") && mine != QLatin1String("owner")) {
                // co-owner равен по правам, снимать такого может только автор
                sendError(socket, 403, "needs_owner",
                          tr("Only the owner can remove a co-owner.")); return;
            }
        }

        QJsonArray kept;
        for (const QJsonValue &value : rows) {
            if (value.toObject().value(QLatin1String("player")).toString() == target) continue;
            kept.append(value);
        }
        m_store->setMembersOfWorld(world, kept);

        sendOk(socket, {{QStringLiteral("ok"), true},
                         {QStringLiteral("world"), world},
                         {QStringLiteral("player"), target},
                         {QStringLiteral("removed"), true}});
        return;
    }

    sendError(socket, 404, "not_found", tr("Unknown endpoint."));
}
