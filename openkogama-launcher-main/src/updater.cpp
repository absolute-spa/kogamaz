#include "updater.h"
#include "appversion.h"
#include "launchdialog.h"
#include "updatecheck.h"
#include "version.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDesktopServices>
#include <QDirIterator>
#include <QEventLoop>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QtConcurrent>
#include <Velopack.hpp>

namespace {

constexpr int ShutdownMs = 2000;
constexpr int SaveMs = 4000;
constexpr int KillMs = 3000;
// Сколько ждать закрытия старого лаунчера перед заменой файлов.
constexpr int WaitExitMs = 30000;

bool isLocalServer(const QProcess *process)
{
    const QString first = process->arguments().value(0);
    return QFileInfo(process->program()).fileName() == "openkogama-server.exe" && first != "tunnel" && first != "install";
}

void stopChildren(QWidget *window)
{
    QNetworkAccessManager net;
    QNetworkRequest request(QUrl(LocalServerUrl + "/shutdown"));
    request.setTransferTimeout(ShutdownMs);
    QNetworkReply *reply = net.get(request);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    reply->deleteLater();
    const QList<QProcess *> processes = window->findChildren<QProcess *>();
    QDeadlineTimer saving(SaveMs);
    for (QProcess *process : processes)
        if (process->state() != QProcess::NotRunning && isLocalServer(process))
            process->waitForFinished(int(saving.remainingTime()));
    for (QProcess *process : processes) {
        if (process->state() == QProcess::NotRunning) continue;
        process->kill();
        process->waitForFinished(KillMs);
    }
}

// --- самозамена портативной сборки -----------------------------------------

// Копирует содержимое src в dst поверх существующего. Старые лишние файлы
// остаются - для лаунчера это безопаснее, чем удалять то, чего мы не знаем.
bool copyOver(const QString &src, const QString &dst, QString *error)
{
    QDirIterator it(src, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString from = it.next();
        const QString relative = QDir(src).relativeFilePath(from);
        const QString to = QDir(dst).filePath(relative);
        if (!QDir().mkpath(QFileInfo(to).absolutePath())) {
            if (error) *error = "Could not create " + QFileInfo(to).absolutePath();
            return false;
        }
        QFile::remove(to);
        if (!QFile::copy(from, to)) {
            if (error) *error = "Could not replace " + to;
            return false;
        }
    }
    return true;
}

}

bool Updater::applyIfNeeded(int &argc, char **argv)
{
    // Аргументы: --apply-update <каталог назначения> <PID старого процесса>
    int index = -1;
    for (int i = 1; i < argc; ++i)
        if (qstrcmp(argv[i], "--apply-update") == 0) { index = i; break; }
    if (index < 0 || index + 2 >= argc) return false;

    const QString target = QString::fromLocal8Bit(argv[index + 1]);
    const qint64 pid = QString::fromLocal8Bit(argv[index + 2]).toLongLong();
    const QString source = QCoreApplication::applicationDirPath();

    // Даём старому экземпляру выйти - иначе exe занят и замена упадёт.
    QDeadlineTimer deadline(WaitExitMs);
    while (!deadline.hasExpired()) {
        QProcess list;
        list.start("tasklist", {"/FI", "PID eq " + QString::number(pid), "/NH"});
        list.waitForFinished(2000);
        if (!list.readAllStandardOutput().contains(QByteArray::number(pid))) break;
        QThread::msleep(300);
    }

    QString error;
    if (!copyOver(source, target, &error)) {
        // Не молчим: игрок должен понять, что обновление не применилось.
        QMessageBox::warning(nullptr, QObject::tr("Update"),
                             QObject::tr("Could not apply the update:\n%1").arg(error));
        return true;
    }

    QProcess::startDetached(QDir(target).filePath(QFileInfo(QCoreApplication::applicationFilePath()).fileName()),
                            {}, target);
    return true;
}

const char *Updater::version()
{
    return AppVersion::String;
}

void Updater::startup() {
    Velopack::VelopackApp::Build().Run();
}

namespace {

struct Update {
    std::shared_ptr<Velopack::UpdateManager> manager;
    std::optional<Velopack::UpdateInfo> info;
    // Заполнено, если лаунчер запущен из portable zip: тогда обновляет себя сам.
    UpdateCheck::Release release;
};

std::shared_ptr<Velopack::UpdateManager> managerFor(bool prerelease) {
    return std::make_shared<Velopack::UpdateManager>(std::make_unique<Velopack::GithubSource>(
        "https://github.com/absolute-spa/kogamaz", "", prerelease));
}

// Блокирующий запрос - вызывается только из QtConcurrent-потока.
QByteArray fetchLatest(QString *error)
{
    QNetworkAccessManager net;
    QNetworkRequest request(QUrl(UpdateCheck::latestUrl()));
    // GitHub требует User-Agent, иначе отвечает 403.
    request.setRawHeader("User-Agent", "KoGaMaZ/" + QByteArray(AppVersion::String));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setTransferTimeout(10000);
    QNetworkReply *reply = net.get(request);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const QByteArray body = reply->readAll();
    if (reply->error() != QNetworkReply::NoError && error)
        *error = reply->errorString();
    reply->deleteLater();
    return body;
}

Update download() {
    Update update;
    // Один блок на весь Velopack: раньше создание менеджера стояло в отдельном
    // try с ранним return, и при исключении портативная сборка не доходила до
    // своего механизма обновления вовсе - лаунчер просто молчал и никогда не
    // узнавал о наших релизах. Теперь любая неудача Velopack ведёт в фолбэк.
    try {
        update.manager = managerFor(false);
        if (update.manager->GetCurrentVersion().find('-') != std::string::npos)
            update.manager = managerFor(true);
        update.info = update.manager->CheckForUpdates();
        if (update.info)
            update.manager->DownloadUpdates(*update.info);
    } catch (const std::exception &error) {
        // Ожидаемый путь для portable zip: Velopack обновляет только то, что
        // установлено их инсталлятором.
        qWarning("Velopack update unavailable: %s", error.what());
        update.info.reset();
    }

    // Свой механизм нужен портативной сборке, и пробуем его ВСЕГДА, когда
    // штатное обновление не дало результата - в том числе когда Velopack не
    // упал, а просто ответил «обновлений нет». Иначе достаточно было бы, чтобы
    // их запрос прошёл без исключения, и лаунчер навсегда перестал бы узнавать
    // о наших релизах - молча.
    if (!update.info) {
        try {
            QString why;
            const QByteArray body = fetchLatest(&why);
            if (why.isEmpty())
                update.release = UpdateCheck::parse(body);
            else
                qWarning("Update check failed: %s", qPrintable(why));
        } catch (const std::exception &fallback) {
            qWarning("Update check failed: %s", fallback.what());
        }
    }
    return update;
}

}

void Updater::check(QWidget *window) {
    auto *watcher = new QFutureWatcher<Update>(window);
    QObject::connect(watcher, &QFutureWatcher<Update>::finished, window, [watcher, window]() {
        Update update = watcher->result();
        watcher->deleteLater();

        // 1. Установленная Velopack-версия: их штатное обновление.
        if (update.info) {
            QString version = QString::fromStdString(update.info->TargetFullRelease.Version);
            if (QMessageBox::question(window, QObject::tr("Update"),
                    QObject::tr("KoGaMaZ %1 is ready. Restart now to update?").arg(version))
                != QMessageBox::Yes) return;
            try {
                stopChildren(window);
                update.manager->WaitExitThenApplyUpdates(*update.info);
                QApplication::quit();
            } catch (const std::exception &error) {
                QMessageBox::warning(window, QObject::tr("Update"), QString::fromUtf8(error.what()));
            }
            return;
        }

        // 2. Портативная сборка: сравниваем версию exe с тегом релиза.
        if (!update.release.isValid()) return;
        if (!Version::isNewer(update.release.version, QString::fromLatin1(AppVersion::String))) return;

        QMessageBox box(window);
        box.setWindowTitle(QObject::tr("Update available"));
        box.setIcon(QMessageBox::Information);
        box.setText(QObject::tr("KoGaMaZ %1 is out (you have %2).")
                        .arg(update.release.version, QString::fromLatin1(AppVersion::String)));
        if (!update.release.notes.isEmpty())
            box.setInformativeText(update.release.notes.left(600));
        QPushButton *later = box.addButton(QObject::tr("Later"), QMessageBox::RejectRole);
        QPushButton *updateNow = box.addButton(QObject::tr("Update"), QMessageBox::AcceptRole);
        box.setDefaultButton(later);
        box.exec();
        if (box.clickedButton() != updateNow) return;

        // Прямой ссылки на zip может не быть (релиз только с Setup.exe) -
        // тогда открываем страницу релиза, ставить вручную.
        if (update.release.zipUrl.isEmpty()) {
            if (!update.release.htmlUrl.isEmpty())
                QDesktopServices::openUrl(QUrl(update.release.htmlUrl));
            return;
        }

        stopChildren(window);
        // Скачивание, распаковка и запуск самозамены - в фоне, чтобы окно
        // не зависло на десятках мегабайт.
        QtConcurrent::run([window, release = update.release]() {
            // Всё, что пошло не так, обязано дойти до игрока: молчаливый отказ
            // («нажал Обновить - и тишина») читается как сломанное обновление
            // лаунчера целиком. Окно показываем через главный поток - из
            // фонового Qt рисовать диалоги нельзя.
            const auto report = [window](const QString &text) {
                QMetaObject::invokeMethod(window, [window, text]() {
                    QMessageBox::warning(window, QObject::tr("Update"), text);
                }, Qt::QueuedConnection);
            };

            QTemporaryDir payload;
            if (!payload.isValid()) {
                report(QObject::tr("Could not create a temporary folder for the update."));
                return;
            }
            const QString zip = payload.filePath("update.zip");

            QNetworkAccessManager net;
            QNetworkRequest request(QUrl(release.zipUrl));
            request.setRawHeader("User-Agent", "KoGaMaZ/" + QByteArray(AppVersion::String));
            QNetworkReply *reply = net.get(request);
            QEventLoop loop;
            QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            loop.exec();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray archive = reply->readAll();
            const QString failure = reply->errorString();
            const bool broken = reply->error() != QNetworkReply::NoError;
            reply->deleteLater();
            // GitHub отвечает на 404/403/5xx телом JSON - это не архив, и без
            // проверки статуса дальше упал бы молча сам tar.
            if (broken || status >= 400 || archive.isEmpty()) {
                report(QObject::tr("Could not download the update (%1).").arg(
                    failure.isEmpty() ? QObject::tr("HTTP %1").arg(status) : failure));
                return;
            }

            QFile file(zip);
            if (!file.open(QIODevice::WriteOnly)) {
                report(QObject::tr("Could not write the update to disk."));
                return;
            }
            const qint64 written = file.write(archive);
            file.close();
            if (written != archive.size()) {
                report(QObject::tr("Not enough disk space for the update."));
                return;
            }

            // Windows 10+ поставляет tar.exe, умеющий распаковывать zip -
            // не тащим в проект свой экстрактор.
            const QString unpack = payload.filePath("unpacked");
            QDir().mkpath(unpack);
            QProcess tar;
            tar.start("tar", {"-xf", zip, "-C", unpack});
            if (!tar.waitForFinished(120000) || tar.exitCode() != 0) {
                report(QObject::tr("The downloaded file is not a valid update archive."));
                return;
            }

            // В zip корень с папкой KoGaMaZ/ - ищем каталог с exe.
            QString root = unpack;
            QDirIterator it(unpack, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString dir = it.next();
                if (QFileInfo::exists(dir + "/KoGaMaZ.exe")) { root = dir; break; }
            }
            if (!QFileInfo::exists(root + "/KoGaMaZ.exe")) {
                report(QObject::tr("The update archive does not contain KoGaMaZ.exe."));
                return;
            }

            const QString appDir = QCoreApplication::applicationDirPath();
            QProcess::startDetached(QDir(root).filePath("KoGaMaZ.exe"),
                                    {"--apply-update", appDir,
                                     QString::number(QCoreApplication::applicationPid())});
            QCoreApplication::quit();
        });
    });
    watcher->setFuture(QtConcurrent::run(download));
}
