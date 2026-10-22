#include "gamelauncher.h"
#include "launchdialog.h"
#include "paths.h"
#include "playerprofile.h"
#include "webplayerruntime.h"
#include "consolewindow.h"
#include "settings.h"
#include "discordpresence.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QProcess>
#include <QUrl>
#include <QUuid>
#include <QWidget>

static QString instanceTitle(const QString &path)
{
    QFile file(QDir(path).filePath("instance.json"));
    QJsonObject info;
    if (file.open(QIODevice::ReadOnly))
        info = QJsonDocument::fromJson(file.readAll()).object();
    for (const char *key : {"name", "version"}) {
        QString value = info.value(key).toString().trimmed();
        if (!value.isEmpty())
            return "KoGaMa " + value;
    }
    return "KoGaMa";
}

static QString presenceDetails(const QString &url, const QString &mode)
{
    if (mode == "avatar")
        return "Editing an avatar";
    if (mode != "play")
        return "Building a world";
    if (url == LocalServerUrl)
        return "Playing a local world";
    const QList<GameServer> servers = Settings::servers();
    for (const GameServer &server : servers)
        if (server.address == url && !server.title.isEmpty())
            return "Playing " + server.title;
    return "Playing online";
}

QProcess *GameLauncher::start(const QString &path, const QString &exe, const QString &url, const QString &mode, int world, QWidget *owner, QWidget *parent, bool viaCode) {
    const QString title = instanceTitle(path);
    const bool local = url == LocalServerUrl;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (local) {
        env.insert("http_proxy", "http://127.0.0.1:8081");
        env.insert("no_proxy", "127.0.0.1,localhost");
    }

    QString session = url + "/session?mode=" + mode + (world ? "&world=" + QString::number(world) : QString());
    const QString nickname = Settings::nickname();
    if (!nickname.isEmpty())
        session += "&name=" + QString::fromLatin1(QUrl::toPercentEncoding(nickname));
    if (viaCode)
        session += "&local=" + QString::number(LocalHttpPort);
    const QString settingsKey = QUuid::createUuid().toString(QUuid::Id128);
    session += "&key=" + settingsKey;

    auto *client = new QProcess(owner);
    QDir logs(QDir(path).filePath("logs"));
    logs.mkpath(".");
    QString log = logs.filePath("client-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz") + ".log");

    if (WebPlayerRuntime::isWebPlayerFile(exe)) {
        QFile meta(QDir(path).filePath("instance.json"));
        QJsonObject info;
        if (meta.open(QIODevice::ReadOnly)) info = QJsonDocument::fromJson(meta.readAll()).object();
        QString version = info.value("version").toString();
        QString file = QDir::toNativeSeparators(QDir(path).filePath(exe));
        client->setProgram(Paths::player());
        if (info.value("unityVersion").toString().startsWith("3.4")) {
            if (mode == "avatar") {
                client->deleteLater();
                QMessageBox::information(parent, "Worlds", "This version has no avatar editor");
                return nullptr;
            }
            const QString username = viaCode && !nickname.isEmpty() ? QString::fromLatin1(QUrl::toPercentEncoding(nickname)) : "1";
            QString query = "?Username=" + username + "&Password=&PlanetName=" + QString::number(world) + "&EditMode=" + (mode == "play" ? "false" : "true");
            client->setArguments({file + query, "--serve-as", url + "/kogama2012/WebPlayer.unity3d",
                "--title", title, "--log", QDir::toNativeSeparators(log)});
        } else {
            client->setArguments({file, "--version", version,
                "--reply", "sendPlayerParams=" + session + "&client=" + version, "--title", title, "--log", QDir::toNativeSeparators(log)});
        }
    } else {
        client->setProgram(QDir(path).filePath(exe));
        QFile meta(QDir(path).filePath("instance.json"));
        QJsonObject info;
        if (meta.open(QIODevice::ReadOnly)) info = QJsonDocument::fromJson(meta.readAll()).object();
        QString version = info.value("version").toString();
        if (!version.isEmpty())
            session += "&client=" + version;
        QString unity = info.value("unityVersion").toString();
        if (!unity.isEmpty())
            session += "&unity=" + unity;
        client->setArguments({"kogamaPackage:" + QString::fromLatin1(session.toUtf8().toBase64()), "-logFile", QDir::toNativeSeparators(log)});
    }
    client->setWorkingDirectory(path);
    client->setProcessEnvironment(env);

    auto *console = new ConsoleWindow("Console - " + title);
    console->setAttribute(Qt::WA_DeleteOnClose);
    console->setAttribute(Qt::WA_QuitOnClose, false);
    console->follow("Game", log);
    if (WebPlayerRuntime::isWebPlayerFile(exe))
        console->followNewest("Unity", QDir::tempPath() + "/UnityWebPlayer/log");
    if (local)
        console->followNewest("Server", Paths::serverData() + "/logs", true);
    QObject::connect(console, &ConsoleWindow::crashed, console, [console]() {
        if (!Settings::consoleOnCrash())
            return;
        console->show();
        console->raise();
    });
    QObject::connect(client, &QProcess::finished, console, [console](int exitCode, QProcess::ExitStatus status) {
        console->gameFinished(exitCode, status);
        if (!console->isVisible())
            console->deleteLater();
    });
    QObject::connect(client, &QProcess::finished, client, [url, settingsKey, local, owner]() { PlayerProfile::keep(url, settingsKey, local, owner); });
    QObject::connect(client, &QProcess::finished, client, &QObject::deleteLater);
    PlayerProfile::send(url, settingsKey, !local);
    client->start();

    if (!client->waitForStarted()) {
        client->deleteLater();
        console->deleteLater();
        QMessageBox::warning(parent, "Worlds", "Could not start " + exe);
        return nullptr;
    }
    DiscordPresence::show(client, presenceDetails(url, mode), title);
    if (Settings::consoleOnLaunch())
        console->show();
    return client;
}
