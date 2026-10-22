#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>

// KoGaMaZ social: thin async client for the social server (see social-server/).
//
// Every callback runs on the GUI thread. `ok` is false when the request failed;
// `error` then carries a human-readable message ready to show in the UI.

namespace Social {

struct Account
{
    QString id;
    QString nick;
    QString token;
    QString handle;   // @username, сервер возвращает его и при входе
};

struct Friend
{
    QString id;
    QString nick;
    QString handle;   // @username - постоянный идентификатор для поиска
    bool online = false;
    QString world;     // what they are playing right now, empty when idle
    QString address;   // p2p:CODE or http://host:port - pass to MainWindow::joinServer
};

struct Request
{
    QString id;
    QString player;
    QString nick;
    QString handle;
};

struct SearchHit
{
    QString id;
    QString nick;
    QString handle;
    QString relation;  // self | friend | outgoing | incoming | none
};

// Кто сейчас хостит открытый мир. Публичный список: зайти можно без кода.
struct Hosted
{
    QString player;
    QString nick;
    QString handle;
    QString title;
    QString address;
};

// Одно сообщение в личной переписке с другом.
struct Message
{
    QString id;
    bool mine = false;    // своё или чужое - рисуем по разные стороны
    QString body;
    qint64 created = 0;
    bool read = false;    // для своих: прочитал ли собеседник
};

// Пост в профиле. Видят друзья и сам автор.
struct Post
{
    QString id;
    QString body;
    qint64 created = 0;
};

// Страница истории: hasMore позволяет догрузить более старые сообщения.
struct ChatPage
{
    QList<Message> messages;
    bool hasMore = false;
};

using Callback = std::function<void(bool ok, const QJsonObject &json, const QString &error)>;

// False when no server address is configured - callers then hide social UI.
bool enabled();

// --- account ---------------------------------------------------------------
// handle - придумываемый самим игроком @username, как в Telegram. Постоянный и
// уникальный, по нему друзья ищут и добавляют; ник можно менять хоть каждый день.
void signUp(const QString &nick, const QString &handle, const QString &password, const Callback &done);
void login(const QString &nick, const QString &password, const Callback &done);
void logout(const Callback &done);
void me(const Callback &done);

// --- people ----------------------------------------------------------------
void search(const QString &nick, const Callback &done);
void friends(const Callback &done);
void requests(const Callback &done);
void sendRequest(const QString &targetId, const Callback &done);
// action is one of: accept, reject, cancel
void answer(const QString &requestId, const QString &action, const Callback &done);

// Heartbeat: tells the server I am online and, while playing, which world and
// which joinable address. Call it every ~30s; offline = no heartbeat for 90s.
// public = true puts the world into the shared list so anyone can join without
// a code; private keeps it visible to friends only.
void presence(const QString &world, const QString &address, bool isPublic, const Callback &done);

// Публичный список открытых миров: кто хостит и по какому адресу зайти без кода.
void hosting(const Callback &done);

// --- chat (Этап 2) ---------------------------------------------------------
// Личная переписка доступна только друзьям - сервер это проверяет сам.
// История листается курсором: before = created последнего загруженного
// сообщения, limit ограничивает страницу.
void chatHistory(const QString &peer, qint64 before, const Callback &done);
void sendMessage(const QString &peer, const QString &body, const Callback &done);
void markRead(const QString &peer, const Callback &done);

// --- posts (Этап 2) --------------------------------------------------------
// Посты профиля: player пустой = мои, иначе id друга (видно только друзьям).
void posts(const QString &player, const Callback &done);
void addPost(const QString &body, const Callback &done);
void deletePost(const QString &postId, const Callback &done);

// --- world collaborators (Этап 3) -------------------------------------------
// Роли соавторов одной карты.
//
// Роли - только учёт в лаунчере: доступ к стройке решает игровой сервер общим
// joinMode на весь сервер, поэтому роль не запрещает строить по-настоящему.
// Всё, что она даёт здесь, - кто кого позвал и кто может звать дальше.
// role при приглашении: co-owner | tester | member. Назначить owner нельзя:
// автор карты и есть owner, и сменить эту роль нельзя.
struct Member
{
    QString player;
    QString nick;
    QString handle;
    QString role;   // owner | co-owner | tester | member
    qint64 created = 0;
    bool self = false;
};

void worldMembers(int world, const Callback &done);
void inviteToWorld(int world, const QString &player, const QString &role, const Callback &done);
void removeWorldMember(int world, const QString &player, const Callback &done);

// --- parsing ---------------------------------------------------------------
Account accountOf(const QJsonObject &json);
QList<Friend> friendListOf(const QJsonObject &json);
QList<Request> requestListOf(const QJsonObject &json, const QString &key);
QList<SearchHit> hitListOf(const QJsonObject &json);
QList<Hosted> hostedListOf(const QJsonObject &json);
QList<Message> messageListOf(const QJsonObject &json);
ChatPage chatPageOf(const QJsonObject &json);
QList<Post> postListOf(const QJsonObject &json);
QList<Member> memberListOf(const QJsonObject &json);

} // namespace Social
