#include "playerprofile.h"
#include "paths.h"
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSaveFile>
#include <algorithm>
#include <functional>

namespace {

constexpr int SendTimeout = 5000;
constexpr int FetchTimeout = 5000;
constexpr int ExportTimeout = 3000;

QString folder()
{
    return Paths::data() + "/profile";
}

QString filePath(const QString &name)
{
    return folder() + "/" + name + ".json";
}

QJsonValue read(const QString &name)
{
    QFile file(filePath(name));
    if (!file.open(QIODevice::ReadOnly))
        return QJsonValue::Null;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? QJsonValue(document.object()) : QJsonValue(QJsonValue::Null);
}

void write(const QString &name, const QJsonValue &value)
{
    if (!value.isObject()) {
        QFile::remove(filePath(name));
        return;
    }
    QDir().mkpath(folder());
    QSaveFile file(filePath(name));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(value.toObject()).toJson());
        file.commit();
    }
}

void fetch(const QString &address, QObject *owner, const std::function<void(const QJsonObject &)> &done)
{
    auto *net = new QNetworkAccessManager(owner);
    QNetworkRequest request{QUrl(address)};
    request.setTransferTimeout(FetchTimeout);
    QNetworkReply *reply = net->get(request);
    QObject::connect(reply, &QNetworkReply::finished, net, [net, reply, done]() {
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        if (reply->error() == QNetworkReply::NoError && document.isObject())
            done(document.object());
        net->deleteLater();
    });
}

void keepExport(const QJsonObject &profile)
{
    write("avatar", profile.value("avatar"));
    write("inventory", profile.value("inventory"));
}

void exportNow()
{
    QNetworkAccessManager net;
    QNetworkRequest request{QUrl(LocalServerUrl + "/api/profile/export")};
    request.setTransferTimeout(ExportTimeout);
    QNetworkReply *reply = net.get(request);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
    if (reply->error() == QNetworkReply::NoError && document.isObject())
        keepExport(document.object());
}

}

namespace PlayerProfile {

void send(const QString &url, const QString &key, bool everything)
{
    QJsonObject package{{"settings", read("settings")}};
    if (everything) {
        exportNow();
        package["avatar"] = read("avatar");
        package["inventory"] = read("inventory");
    }
    if (std::none_of(package.constBegin(), package.constEnd(), [](const QJsonValue &value) { return value.isObject(); }))
        return;

    QNetworkAccessManager net;
    QNetworkRequest request(QUrl(url + "/api/player?key=" + key));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setTransferTimeout(SendTimeout);
    QNetworkReply *reply = net.post(request, QJsonDocument(package).toJson(QJsonDocument::Compact));
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec(QEventLoop::ExcludeUserInputEvents);
}

void keep(const QString &url, const QString &key, bool local, QObject *owner)
{
    fetch(url + "/api/settings?key=" + key, owner, [](const QJsonObject &settings) {
        if (settings.value("Standalone").isObject())
            write("settings", settings);
    });
    if (local)
        refresh(owner);
}

void refresh(QObject *owner)
{
    fetch(LocalServerUrl + "/api/profile/export", owner, keepExport);
}

}
