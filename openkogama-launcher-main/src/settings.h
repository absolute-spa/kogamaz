#pragma once

#include <QList>
#include <QByteArray>
#include <QString>
#include <QStringList>

struct GameServer
{
    QString name;
    QString address;
    QString title;
    QByteArray thumbnail;
    QStringList clients;

    bool accepts(const QString &version) const;
    static QString addressOf(const QString &text);
};

namespace Settings {

QList<GameServer> servers();
void setServers(const QList<GameServer> &servers);
QString publicList();
bool publicView();
void setPublicView(bool shown);
QString currentServer();
void setCurrentServer(const QString &address);

QString lastInstance();
void setLastInstance(const QString &path);

QString nickname();
void setNickname(const QString &name);

// Язык интерфейса: "en" или "ru". Пусто - берём английский, он же язык
// исходников. Выбирается в настройках и применяется к окнам, открытым после
// переключения: готовые окна перерисовывать проще всего заново, чем вручную
// обновлять каждую подпись.
QString language();
void setLanguage(const QString &code);

// KoGaMaZ social: адрес своего соц-сервера (пусто = соц-функции выключены),
// токен сессии и ID аккаунта, выданные сервером после входа или регистрации.
QString socialUrl();
void setSocialUrl(const QString &url);

// Адрес общего сервера, которым пользуются все владельцы лаунчера. Пока пусто
// - каждый запускает свой локальный сервер, и публичный список, друзья и посты
// живут в пределах одной машины. Впишите сюда адрес развёрнутого
// social-server - и все, кто запустит лаунчер, автоматически попадут на общий
// сервер без ручной настройки.
QString sharedSocialUrl();

// Адрес общего соц-сервера. Пусто - общий сервер выключен совсем, лаунчер
// работает только на встроенном локальном. Записывается даже пустая строка,
// иначе выключить его было бы невозможно.
void setSocialShared(const QString &url);
QString socialToken();
void setSocialToken(const QString &token);
QString socialId();
void setSocialId(const QString &id);
// Придуманный игроком @username - постоянный идентификатор, по нему друзья
// ищут и добавляют. Отличается от отображаемого ника, который можно менять.
QString socialHandle();
void setSocialHandle(const QString &handle);

QString rendezvous();
void setRendezvous(const QString &address);
QStringList rendezvousList();
void setRendezvousList(const QStringList &servers);
QString hostSecret();
int hostWorld();
void setHostWorld(int world);

// Публичность хостинга: true — мир виден в общем списке и заходить можно без
// кода, false — только по коду. Выбор живёт в настройках, чтобы не спрашивать
// заново при каждом хостинге.
bool hostPublic();
void setHostPublic(bool isPublic);

QString accentColor();
void setAccentColor(const QString &hex);

QString photonAppId();
void setPhotonAppId(const QString &id);

bool consoleOnLaunch();
bool consoleOnCrash();
void setConsoleOnLaunch(bool enabled);
void setConsoleOnCrash(bool enabled);

bool discordPresence();
void setDiscordPresence(bool enabled);

}
