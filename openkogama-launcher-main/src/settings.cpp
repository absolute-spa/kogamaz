#include "settings.h"
#include "paths.h"
#include "tunnel.h"

#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSettings>
#include <QUrl>
#include <QVersionNumber>
#include <algorithm>

namespace {

QSettings store()
{
    return QSettings(Paths::data() + "/settings.ini", QSettings::IniFormat);
}

// Адрес общего соц-сервера по умолчанию.
const QString SharedSocialDefault = QStringLiteral("__SHARED_SOCIAL_URL__");

// Старый адрес из 1.0.3: домен больше не резолвится. Кто его уже сохранил в
// настройках, тот застрял бы на мёртвом сервере - такие значения сбрасываем.
const QString DeadSharedSocial = QStringLiteral("https://kogamaz.duckdns.org/social");

bool isDeadShared(const QString &url)
{
    QString cleaned = url.trimmed();
    while (cleaned.endsWith('/')) cleaned.chop(1);
    return cleaned.compare(DeadSharedSocial, Qt::CaseInsensitive) == 0;
}

// Ключ токена для сервера. Встроенный сервер каждый запуск берёт новый порт,
// а база у него одна, поэтому все 127.0.0.1 - это один и тот же "local".
QString tokenKey(const QString &url)
{
    if (url.startsWith("http://127.0.0.1")) return QStringLiteral("social/tokens/local");
    QString key = url.toLower();
    key.replace(QRegularExpression("[^a-z0-9]+"), "_");
    return "social/tokens/" + key;
}

}

namespace Settings {

QList<GameServer> servers()
{
    QSettings settings = store();
    QList<GameServer> list;
    int count = settings.beginReadArray("servers");
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        const QString address = settings.value("address").toString().trimmed();
        if (Tunnel::isAddress(address)) {
            list.append({settings.value("name").toString(), Tunnel::address(Tunnel::code(address)),
                settings.value("title").toString(), settings.value("thumbnail").toByteArray(), settings.value("clients").toStringList()});
            continue;
        }
        QUrl url(address.contains("://") ? address : "http://" + address);
        if (!url.isValid() || url.host().isEmpty() || (url.scheme() != "http" && url.scheme() != "https")
            || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment() || url.port() == 0
            || (!url.path().isEmpty() && url.path() != "/")) continue;
        if (!address.contains("://") && url.port() == -1) url.setPort(8080);
        if (url.path() == "/") url.setPath(QString());
        list.append({settings.value("name").toString(), url.toString(QUrl::StripTrailingSlash),
            settings.value("title").toString(), settings.value("thumbnail").toByteArray(), settings.value("clients").toStringList()});
    }
    settings.endArray();
    return list;
}

void setServers(const QList<GameServer> &servers)
{
    QSettings settings = store();
    settings.remove("servers");
    settings.beginWriteArray("servers", servers.size());
    for (int i = 0; i < servers.size(); ++i) {
        settings.setArrayIndex(i);
        settings.setValue("name", servers[i].name);
        settings.setValue("address", servers[i].address);
        settings.setValue("title", servers[i].title);
        settings.setValue("thumbnail", servers[i].thumbnail);
        settings.setValue("clients", servers[i].clients);
    }
    settings.endArray();
}

QString publicList()
{
    // KoGaMaZ: no public openkogama list by default - own servers only.
    return store().value("launcher/serverList", QString()).toString().trimmed();
}

bool publicView()
{
    // Land on "My servers" instead of the public list.
    return store().value("launcher/publicServers", false).toBool();
}

void setPublicView(bool shown)
{
    store().setValue("launcher/publicServers", shown);
}

QString currentServer()
{
    return store().value("launcher/server").toString();
}

void setCurrentServer(const QString &address)
{
    store().setValue("launcher/server", address);
}

QString lastInstance()
{
    return store().value("launcher/lastInstance").toString();
}

void setLastInstance(const QString &path)
{
    store().setValue("launcher/lastInstance", path);
}

QString nickname()
{
    return store().value("player/nickname").toString();
}

void setNickname(const QString &name)
{
    store().setValue("player/nickname", name);
}

QString language()
{
    // Пусто по умолчанию - лаунчер на английском, как и написан исходник.
    // Держим код в отдельном ключе, а не в QLocale: игрок должен выбирать язык
    // сам, а не получать его из системы.
    return store().value("ui/language").toString().trimmed().toLower();
}

void setLanguage(const QString &code)
{
    store().setValue("ui/language", code.trimmed().toLower());
}

QString socialUrl()
{
    // Пусто по умолчанию: пользователь сам указывает адрес своего сервера.
    // Приводим к виду без завершающего слэша, чтобы склеивать маршруты.
    const QString url = store().value("social/url").toString().trimmed();
    if (isDeadShared(url)) return QString();
    QString cleaned = url;
    while (cleaned.endsWith('/')) cleaned.chop(1);
    return cleaned;
}

void setSocialUrl(const QString &url)
{
    store().setValue("social/url", url.trimmed());
}

QString sharedSocialUrl()
{
    // Общий сервер, на который могут смотреть все владельцы лаунчера: общий
    // список хостящих, друзья, посты. Без него каждый работает на своей
    // локальной базе - и друзья не находят друг друга в поиске.
    //
    // Это УСИЛЕНИЕ, а не требование: локальный сервер поднимается в любом
    // случае, поэтому регистрация, вход и чат работают и без DNS, и без VPS.
    // Общий адрес пробуется на старте и включается только если отвечает.
    //
    // Адрес записан доменом, а не IP: IP-адрес VPS не должен попадать в
    // публичный исходник. Один домен обслуживает и rendezvous, и этот сервер.
    //
    // Ключ social/shared отличает два случая: ключа нет - берём адрес по
    // умолчанию; ключ есть, но пустой - общий сервер выключен совсем и
    // работает только встроенный локальный.
    QSettings settings = store();
    if (settings.contains("social/shared")) {
        const QString stored = settings.value("social/shared").toString().trimmed();
        if (!isDeadShared(stored)) return stored;
    }
    return SharedSocialDefault;
}

void setSocialShared(const QString &url)
{
    // Записываем и пустую строку: так общий сервер выключается совсем, а не
    // возвращается к адресу по умолчанию.
    store().setValue("social/shared", url.trimmed());
}

QString socialToken()
{
    // Токен выдаёт конкретный сервер, и другому он не годится: раньше токен
    // встроенного сервера уходил на общий, тот отвечал 401, а лаунчер всё
    // равно считал игрока вошедшим. Теперь у каждого сервера свой токен.
    const QString url = socialUrl();
    if (url.isEmpty()) return QString();
    QSettings settings = store();

    // Миграция с 1.0.3: там токен был один, и выдавал его встроенный сервер -
    // общий в 1.0.3 не отвечал.
    if (settings.contains("social/token")) {
        const QString legacy = settings.value("social/token").toString();
        if (!legacy.isEmpty() && !settings.contains("social/tokens/local"))
            settings.setValue("social/tokens/local", legacy);
        settings.remove("social/token");
    }
    return settings.value(tokenKey(url)).toString();
}

void setSocialToken(const QString &token)
{
    const QString url = socialUrl();
    QSettings settings = store();
    settings.remove("social/token");
    // Сервер не задан - привязать не к чему. Пустой токен тогда значит
    // "выйти отовсюду", непустой просто некуда записать.
    if (url.isEmpty()) {
        if (token.isEmpty()) settings.remove("social/tokens");
        return;
    }
    if (token.isEmpty()) settings.remove(tokenKey(url));
    else settings.setValue(tokenKey(url), token);
}

QString socialId()
{
    return store().value("social/id").toString();
}

void setSocialId(const QString &id)
{
    store().setValue("social/id", id);
}

QString socialHandle()
{
    return store().value("social/handle").toString();
}

void setSocialHandle(const QString &handle)
{
    store().setValue("social/handle", handle);
}

QString rendezvous()
{
    return store().value("p2p/rendezvous").toString();
}

void setRendezvous(const QString &address)
{
    store().setValue("p2p/rendezvous", address.trimmed());
}

QStringList rendezvousList()
{
    // Готовый список серверов при первом запуске: адрес твоего VPS (хостинг)
    // и адреса дата-центров со скоростных тестов — только для замера пинга,
    // чтобы выбрать ближайший регион под аренду. Как только пользователь
    // сохранит свой список, этот дефолт больше не подмешивается.
    static const QStringList Starter = {
        "kogamaz.duckdns.org", // мой VPS. Домен вместо голого IP, чтобы адрес
                              // сервера можно было поменять без пересборки
                              // лаунчера и без обновления у всех друзей.
        "139.162.140.8",    // Frankfurt
        "139.162.130.8",    // Stockholm
        "176.58.107.39",    // London
        "45.33.32.156",     // Amsterdam
        "5.101.50.50",      // Moscow
        "45.79.79.174",     // Helsinki
        "176.58.104.20",    // Paris
    };
    QSettings settings = store();
    if (settings.contains("p2p/rendezvousList"))
        return settings.value("p2p/rendezvousList").toStringList();
    return Starter;
}

void setRendezvousList(const QStringList &servers)
{
    store().setValue("p2p/rendezvousList", servers);
}

QString hostSecret()
{
    QSettings settings = store();
    QString secret = settings.value("p2p/secret").toString();
    if (secret.size() >= 32) return secret;
    const QString letters = "abcdefghijklmnopqrstuvwxyz0123456789";
    secret.clear();
    for (int i = 0; i < 32; ++i)
        secret += letters[QRandomGenerator::system()->bounded(int(letters.size()))];
    settings.setValue("p2p/secret", secret);
    return secret;
}

int hostWorld()
{
    return store().value("p2p/world").toInt();
}

QString photonAppId()
{
    // Photon Engine App ID KoGaMaZ (для будущих мультиплеер-функций: лобби, чат, поиск игроков)
    QSettings settings = store();
    QString id = settings.value("photon/appId").toString().trimmed();
    if (!id.isEmpty()) return id;
    id = "41f826f7-0770-4825-9d91-decd52d9460f";
    settings.setValue("photon/appId", id);
    return id;
}

void setPhotonAppId(const QString &id)
{
    store().setValue("photon/appId", id.trimmed());
}

void setHostWorld(int world)
{
    store().setValue("p2p/world", world);
}

bool hostPublic()
{
    // Публичный по умолчанию: друзья заходят без кода. Приватность — явный выбор.
    return store().value("p2p/public", true).toBool();
}

void setHostPublic(bool isPublic)
{
    store().setValue("p2p/public", isPublic);
}

bool consoleOnLaunch()
{
    return store().value("console/showOnLaunch", false).toBool();
}

bool consoleOnCrash()
{
    return store().value("console/showOnCrash", true).toBool();
}

void setConsoleOnLaunch(bool enabled)
{
    store().setValue("console/showOnLaunch", enabled);
}

void setConsoleOnCrash(bool enabled)
{
    store().setValue("console/showOnCrash", enabled);
}

bool discordPresence()
{
    return store().value("discord/presence", true).toBool();
}

void setDiscordPresence(bool enabled)
{
    store().setValue("discord/presence", enabled);
}

QString accentColor()
{
    // KoGaMaZ: выбранный пользователем цвет акцента интерфейса (#RRGGBB).
    QSettings settings = store();
    QString hex = settings.value("launcher/accent").toString().trimmed();
    static const QRegularExpression valid("^#[0-9a-fA-F]{6}$");
    if (valid.match(hex).hasMatch()) return hex.toLower();
    return "#2f81f7";
}

void setAccentColor(const QString &hex)
{
    static const QRegularExpression valid("^#[0-9a-fA-F]{6}$");
    const QString cleaned = hex.trimmed();
    store().setValue("launcher/accent", valid.match(cleaned).hasMatch() ? cleaned.toLower() : QString("#2f81f7"));
}

}

QString GameServer::addressOf(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (Tunnel::isAddress(trimmed)) return Tunnel::address(Tunnel::code(trimmed));
    const QUrl url(trimmed);
    if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https") || url.host().isEmpty()
        || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment() || url.port() == 0
        || (!url.path().isEmpty() && url.path() != "/"))
        return {};
    return url.toString(QUrl::RemovePath | QUrl::StripTrailingSlash);
}

bool GameServer::accepts(const QString &version) const
{
    if (clients.isEmpty()) return true;
    const QVersionNumber number = QVersionNumber::fromString(version);
    for (const QString &entry : clients) {
        const QStringList range = entry.split('-');
        const QVersionNumber low = QVersionNumber::fromString(range.value(0).trimmed());
        const QVersionNumber high = QVersionNumber::fromString(range.value(1).trimmed());
        if (range.size() == 2 && !low.isNull() && !high.isNull()) {
            if (!number.isNull() && low <= number && number <= high) return true;
        } else if (entry.trimmed() == version) {
            return true;
        }
    }
    return false;
}
