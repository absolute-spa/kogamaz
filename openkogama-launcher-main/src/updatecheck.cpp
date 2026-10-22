#include "updatecheck.h"
#include "version.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

// Адрес по умолчанию. Меняется через setEndpoint - например, чтобы направить
// проверку на свой сервер вместо GitHub.
QString &endpoint()
{
    static QString url;
    return url;
}

UpdateCheck::Release fromObject(const QJsonObject &object)
{
    UpdateCheck::Release release;
    release.tag = object.value(QLatin1String("tag_name")).toString().trimmed();
    release.version = Version::normalize(release.tag);
    release.title = object.value(QLatin1String("name")).toString().trimmed();
    release.notes = object.value(QLatin1String("body")).toString().trimmed();
    release.htmlUrl = object.value(QLatin1String("html_url")).toString().trimmed();

    // Ищем zip среди артефактов: он и есть портативная сборка для самозамены.
    // Если релиз собран без zip (только Setup.exe), обновление предложим
    // открыть страницу в браузере - без прямой ссылки ставить нечего.
    const QJsonArray assets = object.value(QLatin1String("assets")).toArray();
    for (const QJsonValue &value : assets) {
        const QJsonObject asset = value.toObject();
        const QString name = asset.value(QLatin1String("name")).toString();
        if (!name.endsWith(QLatin1String(".zip"), Qt::CaseInsensitive)) continue;
        release.zipUrl = asset.value(QLatin1String("browser_download_url")).toString();
        break;
    }
    return release;
}

}

namespace UpdateCheck {

QString repository()
{
    // Публичный релиз-фид лаунчера. Токен для чтения не нужен.
    return QStringLiteral("https://github.com/absolute-spa/kogamaz");
}

QString latestUrl()
{
    if (!endpoint().isEmpty()) return endpoint();
    return QStringLiteral("https://api.github.com/repos/absolute-spa/kogamaz/releases/latest");
}

void setEndpoint(const QString &url)
{
    endpoint() = url;
}

Release parse(const QByteArray &json)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || document.isNull()) return {};

    // /releases/latest отдаёт объект, /releases - массив. Понимаем оба:
    // массив нужен, если проверку направили на свой маршрут со списком.
    if (document.isObject()) return fromObject(document.object());

    if (document.isArray()) {
        const QJsonArray list = document.array();
        for (const QJsonValue &value : list) {
            const QJsonObject object = value.toObject();
            // Предрелизы (метка в имени версии) не предлагаем как обновление.
            if (object.value(QLatin1String("prerelease")).toBool()) continue;
            Release release = fromObject(object);
            if (release.isValid()) return release;
        }
    }
    return {};
}

}
