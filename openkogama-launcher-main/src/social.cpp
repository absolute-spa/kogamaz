#include "social.h"
#include "settings.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace {

// The first request after a cold start of a free-tier server can take a while,
// so the budget is generous. Presence uses a short one to avoid stacking calls.
constexpr int RequestTimeoutMs = 45000;
constexpr int PresenceTimeoutMs = 8000;

QNetworkAccessManager *manager()
{
    // One manager for the whole application, owned by qApp so it outlives
    // pending replies but is not destroyed by a static destructor order mess.
    static QNetworkAccessManager *instance = new QNetworkAccessManager(QCoreApplication::instance());
    return instance;
}

void call(const QString &method, const QString &route, const QJsonObject &body,
          int timeoutMs, const Social::Callback &done)
{
    if (!done) return;

    const QString base = Settings::socialUrl();
    if (base.isEmpty()) {
        done(false, {}, QObject::tr("No social server is set in Settings."));
        return;
    }

    QUrl url(base + route);
    if (!url.isValid() || url.host().isEmpty()) {
        done(false, {}, QObject::tr("The social server address is not a valid URL."));
        return;
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setTransferTimeout(timeoutMs);
    const QString token = Settings::socialToken();
    if (!token.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + token.toUtf8());

    const QByteArray payload = body.isEmpty() ? QByteArray() : QJsonDocument(body).toJson(QJsonDocument::Compact);
    QNetworkReply *reply = nullptr;
    if (method == "GET") reply = manager()->get(request);
    else if (method == "DELETE") reply = manager()->deleteResource(request);
    else reply = manager()->post(request, payload);

    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, done]() {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray raw = reply->readAll();

        if (reply->error() != QNetworkReply::NoError && status == 0) {
            done(false, {}, QObject::tr("Cannot reach the social server: %1").arg(reply->errorString()));
            return;
        }

        QJsonObject json;
        if (!raw.isEmpty()) {
            const QJsonDocument parsed = QJsonDocument::fromJson(raw);
            json = parsed.isObject() ? parsed.object() : QJsonObject();
        }

        if (status >= 200 && status < 300) {
            done(true, json, QString());
            return;
        }

        QString message = json.value("message").toString();
        if (message.isEmpty())
            message = QObject::tr("The social server replied with an error (%1).").arg(status);
        done(false, json, message);
    });
}

} // namespace

namespace Social {

bool enabled()
{
    return !Settings::socialUrl().isEmpty();
}

void signUp(const QString &nick, const QString &handle, const QString &password, const Callback &done)
{
    call("POST", "/api/register", {{"nick", nick}, {"handle", handle}, {"password", password}},
        RequestTimeoutMs, done);
}

void login(const QString &nick, const QString &password, const Callback &done)
{
    call("POST", "/api/login", {{"nick", nick}, {"password", password}}, RequestTimeoutMs, done);
}

void logout(const Callback &done)
{
    call("POST", "/api/logout", {}, RequestTimeoutMs, done);
}

void me(const Callback &done)
{
    call("GET", "/api/me", {}, RequestTimeoutMs, done);
}

void search(const QString &nick, const Callback &done)
{
    call("GET", "/api/search?nick=" + QUrl::toPercentEncoding(nick.trimmed()), {}, RequestTimeoutMs, done);
}

void friends(const Callback &done)
{
    call("GET", "/api/friends", {}, RequestTimeoutMs, done);
}

void requests(const Callback &done)
{
    call("GET", "/api/requests", {}, RequestTimeoutMs, done);
}

void sendRequest(const QString &targetId, const Callback &done)
{
    call("POST", "/api/requests", {{"targetId", targetId}}, RequestTimeoutMs, done);
}

void answer(const QString &requestId, const QString &action, const Callback &done)
{
    call("POST", "/api/requests/" + requestId + "/" + action, {}, RequestTimeoutMs, done);
}

void presence(const QString &world, const QString &address, bool isPublic, const Callback &done)
{
    call("POST", "/api/presence", {{"world", world}, {"address", address}, {"public", isPublic}},
        PresenceTimeoutMs, done);
}

void hosting(const Callback &done)
{
    call("GET", "/api/hosting", {}, RequestTimeoutMs, done);
}

// --- chat (Этап 2) ---------------------------------------------------------

void chatHistory(const QString &peer, qint64 before, const Callback &done)
{
    QString route = "/api/messages?peer=" + QUrl::toPercentEncoding(peer);
    if (before > 0) route += "&before=" + QString::number(before);
    call("GET", route, {}, RequestTimeoutMs, done);
}

void sendMessage(const QString &peer, const QString &body, const Callback &done)
{
    call("POST", "/api/messages", {{"peer", peer}, {"body", body}}, RequestTimeoutMs, done);
}

void markRead(const QString &peer, const Callback &done)
{
    call("POST", "/api/messages/read", {{"peer", peer}}, PresenceTimeoutMs, done);
}

// --- posts (Этап 2) --------------------------------------------------------

void posts(const QString &player, const Callback &done)
{
    QString route = "/api/posts";
    if (!player.isEmpty()) route += "?player=" + QUrl::toPercentEncoding(player);
    call("GET", route, {}, RequestTimeoutMs, done);
}

void addPost(const QString &body, const Callback &done)
{
    call("POST", "/api/posts", {{"body", body}}, RequestTimeoutMs, done);
}

void deletePost(const QString &postId, const Callback &done)
{
    call("DELETE", "/api/posts/" + QUrl::toPercentEncoding(postId), {}, RequestTimeoutMs, done);
}

// --- world collaborators (Этап 3) ------------------------------------------

void worldMembers(int world, const Callback &done)
{
    call("GET", QString("/api/worlds/%1/members").arg(world), {}, RequestTimeoutMs, done);
}

void inviteToWorld(int world, const QString &player, const QString &role, const Callback &done)
{
    call("POST", QString("/api/worlds/%1/members").arg(world),
         {{"player", player}, {"role", role}}, RequestTimeoutMs, done);
}

void removeWorldMember(int world, const QString &player, const Callback &done)
{
    call("DELETE", QString("/api/worlds/%1/members/%2")
                       .arg(world).arg(QString::fromLatin1(QUrl::toPercentEncoding(player))),
         {}, RequestTimeoutMs, done);
}

Account accountOf(const QJsonObject &json)
{
    return {json.value("id").toString(), json.value("nick").toString(),
        json.value("token").toString(), json.value("handle").toString()};
}

QList<Friend> friendListOf(const QJsonObject &json)
{
    QList<Friend> list;
    const QJsonArray rows = json.value("friends").toArray();
    for (const QJsonValue &value : rows) {
        const QJsonObject row = value.toObject();
        Friend friend_;
        friend_.id = row.value("id").toString();
        friend_.nick = row.value("nick").toString();
        friend_.handle = row.value("handle").toString();
        friend_.online = row.value("online").toBool();
        friend_.world = row.value("world").toString();
        friend_.address = row.value("address").toString();
        list.append(friend_);
    }
    return list;
}

QList<Request> requestListOf(const QJsonObject &json, const QString &key)
{
    QList<Request> list;
    const QJsonArray rows = json.value(key).toArray();
    for (const QJsonValue &value : rows) {
        const QJsonObject row = value.toObject();
        list.append({row.value("id").toString(), row.value("player").toString(),
            row.value("nick").toString(), row.value("handle").toString()});
    }
    return list;
}

QList<SearchHit> hitListOf(const QJsonObject &json)
{
    QList<SearchHit> list;
    const QJsonArray rows = json.value("results").toArray();
    for (const QJsonValue &value : rows) {
        const QJsonObject row = value.toObject();
        list.append({row.value("id").toString(), row.value("nick").toString(),
            row.value("handle").toString(), row.value("relation").toString()});
    }
    return list;
}

QList<Hosted> hostedListOf(const QJsonObject &json)
{
    QList<Hosted> list;
    const QJsonArray rows = json.value("hosting").toArray();
    for (const QJsonValue &value : rows) {
        const QJsonObject row = value.toObject();
        list.append({row.value("player").toString(), row.value("nick").toString(),
            row.value("handle").toString(), row.value("title").toString(), row.value("address").toString()});
    }
    return list;
}

QList<Message> messageListOf(const QJsonObject &json)
{
    QList<Message> list;
    const QJsonArray rows = json.value("messages").toArray();
    for (const QJsonValue &value : rows) {
        const QJsonObject row = value.toObject();
        list.append({row.value("id").toString(), row.value("mine").toBool(),
            row.value("body").toString(),
            static_cast<qint64>(row.value("created").toDouble()), row.value("read").toBool()});
    }
    return list;
}

ChatPage chatPageOf(const QJsonObject &json)
{
    ChatPage page;
    page.messages = messageListOf(json);
    page.hasMore = json.value("hasMore").toBool();
    return page;
}

QList<Post> postListOf(const QJsonObject &json)
{
    QList<Post> list;
    const QJsonArray rows = json.value("posts").toArray();
    for (const QJsonValue &value : rows) {
        const QJsonObject row = value.toObject();
        list.append({row.value("id").toString(), row.value("body").toString(),
            static_cast<qint64>(row.value("created").toDouble())});
    }
    return list;
}

QList<Member> memberListOf(const QJsonObject &json)
{
    QList<Member> list;
    const QJsonArray rows = json.value("members").toArray();
    for (const QJsonValue &value : rows) {
        const QJsonObject row = value.toObject();
        list.append({row.value("player").toString(), row.value("nick").toString(),
            row.value("handle").toString(), row.value("role").toString(),
            static_cast<qint64>(row.value("created").toDouble()), row.value("self").toBool()});
    }
    return list;
}

} // namespace Social
