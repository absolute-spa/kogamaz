#include "serverplugins.h"
#include "paths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace ServerPlugins {

QString install()
{
    const QDir bundled(Paths::server() + "/plugins");
    if (!bundled.exists()) return {};

    const QString target = Paths::serverData() + "/plugins";
    if (!QDir().mkpath(target)) return QStringLiteral("cannot create %1").arg(target);

    QStringList failed;
    const QFileInfoList dlls = bundled.entryInfoList({"*.dll"}, QDir::Files);
    for (const QFileInfo &dll : dlls) {
        const QString destination = QDir(target).filePath(dll.fileName());
        const QFileInfo current(destination);
        // Копируем только изменившиеся: файл может держать работающий сервер.
        if (current.exists() && current.size() == dll.size()
            && current.lastModified() >= dll.lastModified()) continue;
        QFile::remove(destination);
        if (!QFile::copy(dll.filePath(), destination)) failed.append(dll.fileName());
    }
    return failed.isEmpty() ? QString() : QStringLiteral("cannot copy %1").arg(failed.join(", "));
}

}
