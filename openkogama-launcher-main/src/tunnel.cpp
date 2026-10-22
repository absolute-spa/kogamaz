#include "tunnel.h"
#include "paths.h"
#include "settings.h"
#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>

namespace {
constexpr unsigned long CreateNoWindow = 0x08000000;
constexpr int CodeLength = 6;
const QString Letters = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
// Если выбранный rendezvous-адрес мёртвый, туннель молчит вечно и хост не
// поднимается. Через столько мс без результата перезапускаемся на встроенном.
constexpr int RendezvousWatchdogMs = 9000;
// Эти слова означают, что rendezvous-сервер ответил и соединение состоялось.
const QStringList RendezvousOkWords = {"code", "ready", "found", "missing"};
}

QString Tunnel::normalize(const QString &text)
{
    QString code;
    for (const QChar letter : text.trimmed().toUpper()) {
        if (letter == '-' || letter == ' ') continue;
        if (!Letters.contains(letter)) return {};
        code += letter;
    }
    return code.size() == CodeLength ? code : QString();
}

bool Tunnel::isAddress(const QString &address)
{
    return address.startsWith("p2p:") && !normalize(address.mid(4)).isEmpty();
}

QString Tunnel::address(const QString &code)
{
    return "p2p:" + code;
}

QString Tunnel::code(const QString &address)
{
    return normalize(address.mid(4));
}

QString Tunnel::display(const QString &code)
{
    return code.left(3) + "-" + code.mid(3);
}

QStringList Tunnel::clients(const QJsonObject &info)
{
    QStringList clients;
    const QJsonArray listed = info.value("clients").toArray();
    for (const QJsonValue &value : listed) {
        const QString client = value.toString().trimmed().left(64);
        if (!client.isEmpty()) clients.append(client);
    }
    const QString version = info.value("version").toString().trimmed().left(64);
    if (!info.contains("clients") && !version.isEmpty()) clients.append(version);
    return clients;
}

Tunnel *Tunnel::host(const QString &secret, const QString &info, bool listed, QObject *parent)
{
    QStringList arguments{"host", "--secret", secret, "--info", info,
        "--http", QString::number(LocalHttpPort), "--photon", QString::number(LocalPhotonPort)};
    if (listed) arguments.append("--public");
    return new Tunnel(arguments, parent);
}

Tunnel *Tunnel::join(const QString &code, QObject *parent)
{
    return new Tunnel({"join", code}, parent);
}

Tunnel *Tunnel::find(const QString &code, QObject *parent)
{
    return new Tunnel({"find", code}, parent);
}

Tunnel::Tunnel(const QStringList &arguments, QObject *parent)
    : QObject(parent), m_process(nullptr), m_arguments(arguments)
{
    m_watchdog = new QTimer(this);
    m_watchdog->setSingleShot(true);
    // Мёртвый rendezvous молчит вечно: ни `error`, ни конца процесса не будет,
    // а хост так и не поднимется. Таймер — единственный способ это заметить.
    connect(m_watchdog, &QTimer::timeout, this, [this]() { restartOnBuiltIn("no answer"); });
    spawn(true);
}

void Tunnel::spawn(bool withRendezvous)
{
    auto *process = new QProcess(this);
    process->setProgram(QDir(Paths::server()).filePath("openkogama-server.exe"));
    QStringList all = QStringList{"tunnel"} + m_arguments
        + QStringList{"--parent", QString::number(QCoreApplication::applicationPid())};
    if (withRendezvous) {
        const QString rendezvous = Settings::rendezvous();
        if (!rendezvous.isEmpty()) all += {"--rendezvous", rendezvous};
    }
    process->setArguments(all);
    process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CreateNoWindow;
    });

    QProcess *previous = m_process;
    m_process = process;

    connect(process, &QProcess::readyReadStandardOutput, this, [this, process]() {
        // Процесс уже заменён перезапуском — его хвост нас не интересует.
        if (process != m_process) return;
        m_pending += process->readAllStandardOutput();
        for (qsizetype end = m_pending.indexOf('\n'); end >= 0; end = m_pending.indexOf('\n')) {
            const QString line = QString::fromUtf8(m_pending.left(end)).trimmed();
            m_pending.remove(0, end + 1);
            const qsizetype space = line.indexOf(' ');
            const QString word = space < 0 ? line : line.left(space);
            const QString rest = space < 0 ? QString() : line.mid(space + 1);
            if (RendezvousOkWords.contains(word)) {
                // rendezvous ответил и соединение состоялось — таймер больше не нужен
                m_rendezvousOk = true;
                m_watchdog->stop();
            } else if (word == "error" && rest.startsWith("rendezvous") && !m_rendezvousOk) {
                restartOnBuiltIn(rest);
                return;
            }
            emit said(word, rest);
        }
    });

    connect(process, &QProcess::finished, this, [this, process]() {
        if (process != m_process) return;   // устаревший экземпляр, его ждёт deleteLater
        // Выбранного rendezvous нет, процесс вышел молча — повторим на встроенном.
        if (!m_rendezvousOk && !Settings::rendezvous().isEmpty()) {
            restartOnBuiltIn("exited without a result");
            return;
        }
        m_watchdog->stop();
        m_ended = true;
        emit ended();
    });

    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (process != m_process || error != QProcess::FailedToStart || m_ended) return;
        m_watchdog->stop();
        m_ended = true;
        emit said("error", "Could not start " + process->program());
        emit ended();
    });

    if (withRendezvous && !Settings::rendezvous().isEmpty() && !m_rendezvousOk)
        m_watchdog->start(RendezvousWatchdogMs);

    QMetaObject::invokeMethod(process, [process]() { process->start(); }, Qt::QueuedConnection);

    if (previous) {
        if (previous->state() != QProcess::NotRunning) {
            previous->kill();
            // удаляем по фактическому завершению: kill() асинхронен, и deleteLater()
            // иначе уничтожает объект, за которым ещё присматривает QProcess
            connect(previous, &QProcess::finished, previous, &QObject::deleteLater);
        } else {
            previous->deleteLater();
        }
    }
}

void Tunnel::restartOnBuiltIn(const QString &why)
{
    if (m_retried || m_ended) return;
    m_retried = true;
    m_watchdog->stop();
    m_pending.clear();
    const QString chosen = Settings::rendezvous();
    // Информируем UI, но не роняем туннель: через мгновение пойдёт результат.
    emit said("note", "P2P server " + chosen + " did not respond (" + why
        + "), falling back to the built-in servers");
    spawn(false);
}

void Tunnel::stop()
{
    m_watchdog->stop();
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        // kill() асинхронен: дожидаемся фактического выхода, иначе QProcess
        // выбросит «Destroyed while process is still running» при уничтожении
        if (!m_process->waitForFinished(1500)) m_process->terminate();
    }
}
