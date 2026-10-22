#include "serverspage.h"
#include "settings.h"
#include "settingsdialog.h"
#include "friendsdialog.h"
#include "chatdialog.h"
#include "social.h"
#include "tunnel.h"
#include "sharebar.h"
#include "serverconfigdialog.h"
#include "paths.h"
#include "gamelauncher.h"
#include "centereddelegate.h"
#include "brandlogo.h"
#include "playerstats.h"
#include "profilepage.h"
#include "mainwindow.h"
#include "version.h"
#include "updatecheck.h"
#include "worldmembersdialog.h"
#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QSettings>
#include <QStackedWidget>
#include <QTabBar>
#include <QTableWidget>
#include <QToolButton>
#include <QProgressBar>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QSignalSpy>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QTreeWidget>
#include <QUrl>
#include <QUrlQuery>

class MockServer : public QTcpServer
{
public:
    QByteArray metadata = "{\"title\":\"A friend's server\",\"thumbnail\":\"/api/server/thumbnail\"}";
    QByteArray image;
    QByteArray list = "[]";
    int metadataStatus = 200;
    int imageStatus = 200;
    QStringList requests;

    MockServer()
    {
        listen(QHostAddress::LocalHost);
        QImage tile(8, 8, QImage::Format_RGB32);
        tile.fill(Qt::red);
        QBuffer buffer(&image);
        buffer.open(QIODevice::WriteOnly);
        tile.save(&buffer, "PNG");
        connect(this, &QTcpServer::newConnection, this, [this]() {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, data = QByteArray()]() mutable {
                    data += socket->readAll();
                    if (!data.contains("\r\n\r\n")) return;
                    const QString path = QString::fromLatin1(data.split(' ').value(1));
                    requests.append(path);
                    int status = path == "/api/server" ? metadataStatus : path == "/api/server/thumbnail" ? imageStatus : 404;
                    QByteArray body = path == "/api/server" ? metadata : path == "/api/server/thumbnail" ? image : QByteArray();
                    if (path == "/ping") { status = 200; body = "{}"; }
                    if (path == "/servers") { status = 200; body = list; }
                    socket->write("HTTP/1.1 " + QByteArray::number(status) + " Test\r\nContent-Length: "
                        + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                    data.clear();
                });
            }
        });
    }

    QString url() const { return "http://127.0.0.1:" + QString::number(serverPort()); }
};

static QString tile(const QListWidgetItem *item)
{
    return item->text() + "\n" + item->data(CenteredDelegate::SubtitleRole).toString();
}

// Мини-версия social-server: отвечает на соц-маршруты тем JSON-ом, который
// задал тест, и запоминает входящие запросы (путь + тело).
class SocialMock : public QTcpServer
{
public:
    std::function<QByteArray(const QString &path, const QByteArray &body)> route;
    QStringList requests;
    QStringList bodies;
    // Код ответа, которым отвечает мок. Позволяет проверить, как клиент
    // разбирает отказ сервера, а не только путь успеха.
    int status = 200;
    // Нужен, когда разные пути отвечают по-разному: например, история
    // читается успешно, а отправка отклоняется по лимиту.
    std::function<int(const QString &path)> statusCode;

    SocialMock()
    {
        listen(QHostAddress::LocalHost);
        connect(this, &QTcpServer::newConnection, this, [this]() {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, data = QByteArray()]() mutable {
                    data += socket->readAll();
                    const int head = data.indexOf("\r\n\r\n");
                    if (head < 0) return;
                    const QByteArray headers = data.left(head);
                    int length = 0;
                    for (const QByteArray &line : headers.split('\n')) {
                        const QByteArray trimmed = line.trimmed();
                        if (trimmed.toLower().startsWith("content-length:"))
                            length = trimmed.mid(trimmed.indexOf(':') + 1).trimmed().toInt();
                    }
                    if (data.size() < head + 4 + length) return;   // ждём тело POST-а

                    const QString path = QString::fromLatin1(data.split(' ').value(1));
                    const QByteArray body = data.mid(head + 4, length);
                    requests.append(path);
                    bodies.append(QString::fromUtf8(body));
                    const QByteArray payload = route ? route(path, body) : QByteArray("{}");
                    // код ответа настраивается тестом: без него нельзя проверить
                    // разбор отказа, а только путь успеха
                    const int code = statusCode ? statusCode(path) : status;
                    socket->write("HTTP/1.1 " + QByteArray::number(code) + " Test\r\n"
                        "Content-Type: application/json\r\nContent-Length: "
                        + QByteArray::number(payload.size()) + "\r\nConnection: close\r\n\r\n" + payload);
                    socket->disconnectFromHost();
                    data.clear();
                });
            }
        });
    }

    QString url() const { return "http://127.0.0.1:" + QString::number(serverPort()); }

    static QByteArray json(const QJsonObject &object)
    {
        return QJsonDocument(object).toJson(QJsonDocument::Compact);
    }
};

class ServersPageTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QApplication::setOrganizationName("KoGaMaZTests");
        QApplication::setApplicationName("Launcher-" + QString::number(QApplication::applicationPid()));
    }

    void init()
    {
        Settings::setPublicView(false);
        Settings::setRendezvous(QString());
        // Общий соц-сервер в тестах выключен совсем: он бы уходил в реальную
        // сеть, а здесь важен локальный путь и контракт API.
        Settings::setSocialShared(QString());
        Settings::setSocialUrl(QString());
        Settings::setSocialToken(QString());
        Settings::setSocialId(QString());
        Settings::setSocialHandle(QString());
        Settings::setHostPublic(true);
        QSettings(Paths::data() + "/settings.ini", QSettings::IniFormat).setValue("launcher/serverList", "http://127.0.0.1:9");
    }

    void cleanup()
    {
        Settings::setServers({});
        Settings::setCurrentServer({});
        Settings::setRendezvous(QString());
        Settings::setSocialShared(QString());
        Settings::setSocialUrl(QString());
        Settings::setSocialToken(QString());
        Settings::setSocialId(QString());
        Settings::setSocialHandle(QString());
        Settings::setHostPublic(true);
        Settings::setNickname(QString());
    }

    void metadataJoinAndOfflineCache()
    {
        MockServer server;
        QVERIFY(server.isListening());
        Settings::setServers({{"My friend", server.url(), {}, {}}});
        QByteArray cached;
        {
            ServersPage page;
            auto *list = page.findChild<QListWidget *>("serversList");
            QVERIFY(list);
            QTRY_VERIFY(tile(list->item(0)).contains("Online"));
            QTRY_VERIFY(!Settings::servers().first().thumbnail.isEmpty());
            QCOMPARE(tile(list->item(0)), QString("A friend's server\nOnline"));
            QCOMPARE(server.requests, QStringList({"/api/server", "/api/server/thumbnail"}));
            cached = Settings::servers().first().thumbnail;
            QCOMPARE(QImage::fromData(cached).size(), QSize(400, 160));
            QSignalSpy joined(&page, &ServersPage::joinRequested);
            auto *join = page.findChild<QPushButton *>("joinServerButton");
            QVERIFY(!join->isEnabled());
            page.setCanJoin(true);
            join->click();
            QCOMPARE(joined.size(), 1);
            QCOMPARE(joined.first().first().toString(), server.url());
            server.close();
            emit page.findChild<QShortcut *>("refreshServersShortcut")->activated();
            QTRY_COMPARE(tile(list->item(0)), QString("A friend's server\nOffline"));
            QCOMPARE(Settings::servers().first().thumbnail, cached);
        }
        ServersPage reopened;
        auto *list = reopened.findChild<QListWidget *>("serversList");
        QTRY_COMPARE(tile(list->item(0)), QString("A friend's server\nOffline"));
        QCOMPARE(Settings::servers().first().thumbnail, cached);
    }

    void rejectsOversizedImageAndTruncatesTitle()
    {
        MockServer server;
        server.metadata = QJsonDocument(QJsonObject{{"title", QString(80, 'A')},
            {"thumbnail", "/api/server/thumbnail"}, {"worlds", "ignored"}}).toJson();
        server.image = QByteArray(2 * 1024 * 1024 + 1, 'X');
        Settings::setServers({{"Saved", server.url(), {}, {}}});
        ServersPage page;
        auto *list = page.findChild<QListWidget *>("serversList");
        QTRY_COMPARE(Settings::servers().first().title.size(), 40);
        QTRY_VERIFY(server.requests.contains("/api/server/thumbnail"));
        QTest::qWait(100);
        QVERIFY(Settings::servers().first().thumbnail.isEmpty());
        QCOMPARE(tile(list->item(0)), QString(40, 'A') + "\nOnline");
        QCOMPARE(server.requests.size(), 2);
    }

    void rejectsInvalidImagesAndExternalThumbnailURLs()
    {
        MockServer server;
        server.image = "not an image";
        Settings::setServers({{"Saved", server.url(), {}, {}}});
        ServersPage page;
        QTRY_VERIFY(server.requests.contains("/api/server/thumbnail"));
        QTest::qWait(100);
        QVERIFY(Settings::servers().first().thumbnail.isEmpty());
        server.requests.clear();
        server.metadata = "{\"title\":\"Safe\",\"thumbnail\":\"https://example.com/image.png\"}";
        emit page.findChild<QShortcut *>("refreshServersShortcut")->activated();
        QTRY_COMPARE(Settings::servers().first().title, QString("Safe"));
        QCOMPARE(server.requests, QStringList({"/api/server"}));
    }

    void oldServerUsesOnlyPingFallback()
    {
        MockServer server;
        server.metadataStatus = 404;
        Settings::setServers({{"Old server", server.url(), {}, {}}});
        ServersPage page;
        auto *list = page.findChild<QListWidget *>("serversList");
        QTRY_COMPARE(tile(list->item(0)), QString("Old server\nOnline"));
        QCOMPARE(server.requests, QStringList({"/api/server", "/ping"}));
    }

    void preservesUnicodeTitles()
    {
        MockServer server;
        server.metadata = QJsonDocument(QJsonObject{{"title", QString::fromUtf8("😀").repeated(45)}}).toJson();
        Settings::setServers({{"Saved", server.url(), {}, {}}});
        ServersPage page;
        QTRY_COMPARE(Settings::servers().first().title.toUcs4().size(), 40);
    }

    void publicListShowsAndSavesServers()
    {
        MockServer server;
        server.list = QJsonDocument(QJsonArray{
            QJsonObject{{"address", server.url()}, {"title", "Public one"}, {"clients", QJsonArray{"3.5.20"}}, {"players", 3}, {"thumbnail", true}},
            QJsonObject{{"address", "file:///etc/passwd"}, {"title", "Bad"}},
            QJsonObject{{"address", "p2p:abcdef"}, {"title", "Hosted"}, {"players", 1}},
        }).toJson();
        QSettings(Paths::data() + "/settings.ini", QSettings::IniFormat).setValue("launcher/serverList", server.url());
        Settings::setPublicView(true);
        {
            ServersPage page;
            auto *list = page.findChild<QListWidget *>("serversList");
            QTRY_COMPARE(list->count(), 2);
            QCOMPARE(tile(list->item(0)), QString::fromUtf8("Public one\n3 players · 3.5.20"));
            QCOMPARE(tile(list->item(1)), QString("Hosted\n1 player"));
            QVERIFY(page.isPublic(server.url()));
            QVERIFY(page.isPublic("p2p:ABCDEF"));
            QCOMPARE(page.clientsOf(server.url()), QStringList({"3.5.20"}));
            QTRY_VERIFY(server.requests.contains("/api/server/thumbnail"));
            list->setCurrentRow(0);
            QAction *save = nullptr;
            for (QAction *action : page.findChild<QToolButton *>("moreServerButton")->menu()->actions())
                if (action->text() == "Save to my servers") save = action;
            QVERIFY(save && save->isVisible() && save->isEnabled());
            save->trigger();
            QCOMPARE(Settings::servers().size(), 1);
            QCOMPARE(Settings::servers().first().title, QString("Public one"));
            QCOMPARE(Settings::servers().first().clients, QStringList({"3.5.20"}));
            QVERIFY(!save->isEnabled());
            QObject game;
            page.reportPlaying(&game, server.url());
            QTRY_VERIFY(server.requests.contains("/playing"));
            page.findChild<QTabBar *>("serversViewTabs")->setCurrentIndex(1);
            QCOMPARE(list->count(), 1);
            QCOMPARE(list->item(0)->text(), QString("Public one"));
        }
        QVERIFY(!Settings::publicView());
        QSettings(Paths::data() + "/settings.ini", QSettings::IniFormat).remove("launcher/serverList");
    }

    void joinStartsGameWithServerSession()
    {
        QTemporaryDir instance;
        QVERIFY(instance.isValid());
        QFile meta(instance.filePath("instance.json"));
        QVERIFY(meta.open(QIODevice::WriteOnly));
        meta.write("{\"name\":\"Test\",\"version\":\"2015\",\"unityVersion\":\"5.2\"}");
        meta.close();
        QWidget owner;
        QPointer<QProcess> client = GameLauncher::start(instance.path(), QApplication::applicationFilePath(),
            "http://127.0.0.1:8080", "play", 0, &owner, &owner);
        QVERIFY(client);
        const QString capture = instance.filePath("launch-capture.json");
        QTRY_VERIFY(QFile::exists(capture));
        QFile result(capture);
        QVERIFY(result.open(QIODevice::ReadOnly));
        const QJsonObject launched = QJsonDocument::fromJson(result.readAll()).object();
        const QUrl session(launched.value("session").toString());
        const QUrlQuery query(session);
        QCOMPARE(session.toString(QUrl::RemoveQuery), QString("http://127.0.0.1:8080/session"));
        QCOMPARE(query.queryItemValue("mode"), QString("play"));
        QCOMPARE(query.queryItemValue("client"), QString("2015"));
        QCOMPARE(query.queryItemValue("unity"), QString("5.2"));
        QVERIFY(!query.queryItemValue("key").isEmpty());
        QCOMPARE(launched.value("cwd").toString(), instance.path());
        QTRY_VERIFY(!client || client->state() == QProcess::NotRunning);
    }

    void codeShowsHostedWorld()
    {
        QCOMPARE(Tunnel::normalize("abc-def"), QString("ABCDEF"));
        QCOMPARE(Tunnel::normalize(" ABC DEF "), QString("ABCDEF"));
        QVERIFY(Tunnel::normalize("ABCDE1").isEmpty());
        QVERIFY(Tunnel::normalize("ABCDEFG").isEmpty());
        QCOMPARE(Tunnel::display("ABCDEF"), QString("ABC-DEF"));
        const QString server = QApplication::applicationDirPath() + "/server";
        QVERIFY(QDir().mkpath(server));
        QFile::remove(server + "/openkogama-server.exe");
        QVERIFY(QFile::copy(QApplication::applicationFilePath(), server + "/openkogama-server.exe"));
        Settings::setServers({{"Code ABC-DEF", "p2p:abc-def", {}, {}}});
        QCOMPARE(Settings::servers().first().address, QString("p2p:ABCDEF"));
        {
            ServersPage page;
            auto *list = page.findChild<QListWidget *>("serversList");
            QTRY_COMPARE(tile(list->item(0)), QString::fromUtf8("Fake world (Ann)\nOnline · 3.5.20"));
            QCOMPARE(Settings::servers().first().clients, QStringList({"3.5.20"}));
            QCOMPARE(page.findChild<QLabel *>("selectedServerAddress")->text(), QString("ABC-DEF"));
        }
        QDir(server).removeRecursively();
    }

    void hostingShowsCodeAndFriends()
    {
        const QString server = QApplication::applicationDirPath() + "/server";
        QVERIFY(QDir().mkpath(server));
        QFile::remove(server + "/openkogama-server.exe");
        QVERIFY(QFile::copy(QApplication::applicationFilePath(), server + "/openkogama-server.exe"));
        {
            ShareBar bar;
            bar.share(4, "Fake world", "3.5.20");
            QVERIFY(!bar.isHidden());
            QTRY_COMPARE(bar.findChild<QLabel *>("shareCode")->text(), QString("Code ABC-DEF"));
            QTRY_COMPARE(bar.findChild<QLabel *>("shareInfo")->text(), QString("1 player joined"));
            QCOMPARE(bar.players(), 1);
            bar.findChild<QPushButton *>("shareStopButton")->click();
            QVERIFY(bar.isHidden());
            QCOMPARE(bar.players(), 0);
        }
        QDir(server).removeRecursively();
    }

    // Выбранный пользователем rendezvous без сервиса matching молчит вечно:
    // раньше хост просто не поднимался. Теперь туннель сам откатывается на
    // встроенные P2P-серверы и всё равно выдаёт код.
    void deadRendezvousFallsBackToBuiltIn()
    {
        const QString server = QApplication::applicationDirPath() + "/server";
        QVERIFY(QDir().mkpath(server));
        QFile::remove(server + "/openkogama-server.exe");
        QVERIFY(QFile::copy(QApplication::applicationFilePath(), server + "/openkogama-server.exe"));

        Settings::setRendezvous("10.255.255.1:5058");
        {
            ShareBar bar;
            bar.share(4, "Fake world", "3.5.20");
            auto *info = bar.findChild<QLabel *>("shareInfo");
            QVERIFY(info);
            // туннель сообщает о фоллбэке, а не висит молча
            QTRY_VERIFY_WITH_TIMEOUT(info->text().contains("falling back"), 25000);
            // и код всё равно появился через встроенные серверы
            QTRY_COMPARE_WITH_TIMEOUT(bar.findChild<QLabel *>("shareCode")->text(),
                                      QString("Code ABC-DEF"), 25000);
            bar.findChild<QPushButton *>("shareStopButton")->click();
            QVERIFY(bar.isHidden());
        }
        Settings::setRendezvous(QString());
        QDir(server).removeRecursively();
    }

    // Доверие к выбору сервера: строка Auto всегда доступна, а адрес без сервиса
    // matching честно помечается в колонке P2P service и сбрасывается при сохранении.
    void settingsDialogMarksServersWithoutP2PService()
    {
        const QStringList list{"64.176.22.9", "139.162.140.8"};
        Settings::setRendezvousList(list);
        Settings::setRendezvous(list.first());   // битый активный адрес, как в ине пользователя

        SettingsDialog dialog;
        dialog.show();
        auto *table = dialog.findChild<QTableWidget *>("rendezvousTable");
        QVERIFY(table);
        QCOMPARE(table->columnCount(), 4);

        // строка 0 — Auto, не удаляется и хранит пустой адрес
        QVERIFY(table->item(0, 0)->text().contains("Auto"));
        QCOMPARE(table->rowCount(), list.size() + 1);
        QCOMPARE(table->currentRow(), 1);        // активный адрес из ине

        // UDP-проба 5058 честно показывает, что сервиса на адресе нет
        QTRY_COMPARE_WITH_TIMEOUT(table->item(1, 3)->text(), QString("no"), 15000);
        auto *status = dialog.findChild<QLabel *>("p2pStatus");
        QVERIFY(status);
        QVERIFY(status->text().contains("No KoGaMaZ P2P service"));

        // визуальный артефакт: так удобно проверять вёрстку таблицы колонок
        const QString shot = QDir::tempPath() + "/kogamaz-settings-dialog.png";
        QVERIFY(dialog.grab().save(shot));

        // строку Auto удалить нельзя — это безопасный дефолт
        auto *remove = dialog.findChild<QPushButton *>("removeServerButton");
        QVERIFY(remove);
        table->selectRow(0);
        const int rows = table->rowCount();
        remove->click();
        QCOMPARE(table->rowCount(), rows);

        // Add добавляет строку и сразу открывает редактирование адреса
        auto *add = dialog.findChild<QPushButton *>("addServerButton");
        QVERIFY(add);
        add->click();
        QCOMPARE(table->rowCount(), rows + 1);
        QVERIFY2(table->findChild<QLineEdit *>(), "Add must open an editor for the new address");

        // сохранились ровно адреса списка, а встроенные серверы оставлены выбранными
        table->selectRow(0);
        dialog.accept();
        QVERIFY(Settings::rendezvous().isEmpty());
        QCOMPARE(Settings::rendezvousList(), list);

        Settings::setRendezvousList(QStringList());
    }

    void settingsDialogSignsInToSocialAccount()
    {
        SocialMock server;
        server.route = [](const QString &path, const QByteArray &body) {
            if (path == "/api/register")
                return SocialMock::json({{"token", "tok-new"}, {"id", "id-alice"}, {"nick", "Alice"}, {"handle", "alice"}});
            if (path == "/api/login")
                return SocialMock::json({{"token", "tok-alice"}, {"id", "id-alice"}, {"nick", "Alice"}, {"handle", "alicedev"}});
            if (path == "/api/me")
                return SocialMock::json({{"id", "id-alice"}, {"nick", "Alice"}, {"handle", "alicedev"}});
            return QByteArray("{}");
        };

        SettingsDialog dialog;
        auto *url = dialog.findChild<QLineEdit *>("socialUrlEdit");
        auto *password = dialog.findChild<QLineEdit *>("socialPasswordEdit");
        auto *status = dialog.findChild<QLabel *>("socialStatus");
        auto *signIn = dialog.findChild<QPushButton *>("socialSignInButton");
        auto *signOut = dialog.findChild<QPushButton *>("socialSignOutButton");
        QVERIFY(url && password && status && signIn && signOut);

        // без адреса сервера соц-функции честно считаются выключенными
        QVERIFY(status->text().contains("off"));
        QVERIFY(!signOut->isEnabled());

        url->setText(server.url());
        url->editingFinished();
        QCOMPARE(Settings::socialUrl(), server.url());
        QVERIFY(status->text().contains("Not signed in"));

        // пустые ник и пароль не должны уходить на сервер
        signIn->click();
        QVERIFY(status->text().contains("nickname"));
        QVERIFY(server.requests.isEmpty());

        // nicknameEdit — общее поле из .ui, вводим ник туда
        auto *nick = dialog.findChild<QLineEdit *>("nicknameEdit");
        QVERIFY(nick);
        nick->setText("  Alice  ");
        password->setText("hunter2");
        signIn->click();
        QTRY_COMPARE(Settings::socialToken(), QString("tok-alice"));
        QCOMPARE(Settings::socialId(), QString("id-alice"));
        QCOMPARE(Settings::socialHandle(), QString("alicedev"));   // @username подтверждён сервером
        QCOMPARE(Settings::nickname(), QString("Alice"));     // пробелы обрезаны
        QVERIFY(status->text().contains("Signed in as Alice"));
        QVERIFY(status->text().contains("@alicedev"));        // @username виден в статусе
        QVERIFY(server.requests.contains("/api/login"));
        QVERIFY(signOut->isEnabled());      // вошли — выход разрешён

        // выход чистит токен локально
        signOut->click();
        QTRY_VERIFY(Settings::socialToken().isEmpty());
        QVERIFY(Settings::socialId().isEmpty());
        QVERIFY(status->text().contains("Signed out"));
    }

    void signUpRequiresAValidUsername()
    {
        SocialMock server;
        server.route = [](const QString &path, const QByteArray &) {
            if (path == "/api/register")
                return SocialMock::json({{"token", "tok-new"}, {"id", "id-a"}, {"nick", "Alice"}, {"handle", "alice"}});
            return QByteArray("{}");
        };
        Settings::setSocialUrl(server.url());

        SettingsDialog dialog;
        auto *nick = dialog.findChild<QLineEdit *>("nicknameEdit");
        auto *handle = dialog.findChild<QLineEdit *>("socialHandleEdit");
        auto *password = dialog.findChild<QLineEdit *>("socialPasswordEdit");
        auto *signUp = dialog.findChild<QPushButton *>("socialSignUpButton");
        auto *status = dialog.findChild<QLabel *>("socialStatus");
        QVERIFY(nick && handle && password && signUp && status);
        nick->setText("Alice");
        password->setText("hunter2");

        // без @username регистрация не уходит на сервер
        signUp->click();
        QVERIFY(status->text().contains("username"));
        QVERIFY(!server.requests.contains("/api/register"));

        // запрещённые символы отсекаются локально, до поездки на сервер
        handle->setText("Alice Dev!");
        signUp->click();
        QVERIFY(status->text().contains("letters, digits"));
        QVERIFY(!server.requests.contains("/api/register"));

        // слишком короткий тоже
        handle->setText("ab");
        signUp->click();
        QVERIFY(status->text().contains("letters, digits"));
        QVERIFY(!server.requests.contains("/api/register"));

        // "@" спереди убирается, регистр сводится к нижнему
        handle->setText("@AliceDev");
        signUp->click();
        QTRY_VERIFY(server.requests.contains("/api/register"));
        // токен и @username пишутся только в ответе сервера, дожидаемся их
        QTRY_COMPARE(Settings::socialHandle(), QString("alicedev"));
        QTRY_COMPARE(Settings::socialToken(), QString("tok-new"));
        QVERIFY(status->text().contains("@alice"));
    }

    void friendsDialogShowsRequestsFriendsAndJoin()
    {
        SocialMock server;
        server.route = [](const QString &path, const QByteArray &) {
            if (path.startsWith("/api/search")) {
                QJsonArray results;
                results.append(QJsonObject{{"id", "id-bob"}, {"nick", "Bob"}, {"handle", "bobdev"}, {"relation", "none"}});
                results.append(QJsonObject{{"id", "id-carol"}, {"nick", "Carol"}, {"handle", "carol"}, {"relation", "outgoing"}});
                return SocialMock::json({{"results", results}});
            }
            if (path == "/api/friends") {
                QJsonArray friends;
                friends.append(QJsonObject{{"id", "id-dave"}, {"nick", "Dave"}, {"handle", "davedev"}, {"online", true},
                                           {"world", "My World"}, {"address", "p2p:ABCDEF"}});
                friends.append(QJsonObject{{"id", "id-eve"}, {"nick", "Eve"}, {"handle", "eve"}, {"online", false},
                                           {"world", ""}, {"address", ""}});
                return SocialMock::json({{"friends", friends}});
            }
            if (path == "/api/requests") {
                QJsonArray incoming;
                incoming.append(QJsonObject{{"id", "req-1"}, {"player", "id-frank"}, {"nick", "Frank"}, {"handle", "frank"}});
                QJsonArray outgoing;
                outgoing.append(QJsonObject{{"id", "req-2"}, {"player", "id-grace"}, {"nick", "Grace"}, {"handle", "grace"}});
                return SocialMock::json({{"incoming", incoming}, {"outgoing", outgoing}});
            }
            if (path == "/api/hosting") {
                QJsonArray hosting;
                hosting.append(QJsonObject{{"player", "id-hana"}, {"nick", "Hana"}, {"handle", "hanadev"},
                                            {"title", "De_Dust2 CSGO"}, {"address", "p2p:HANA01"}});
                return SocialMock::json({{"hosting", hosting}});
            }
            return QByteArray("{\"ok\":true}");
        };

        Settings::setSocialUrl(server.url());
        Settings::setSocialToken("tok-alice");
        Settings::setSocialId("id-alice");
        Settings::setSocialHandle("alicedev");
        Settings::setNickname("Alice");

        FriendsDialog dialog;
        dialog.show();

        // @username виден в шапке - по нему друзья ищут этого игрока
        auto *accountLine = dialog.findChild<QLabel *>("accountLine");
        QVERIFY(accountLine);
        QTRY_VERIFY(accountLine->text().contains("@alicedev"));

        // список друзей: онлайн-друг с миром, офлайн-друг без него
        auto *friends = dialog.findChild<QListWidget *>("friendsList");
        QVERIFY(friends);
        QTRY_COMPARE(friends->count(), 2);
        auto *online = qobject_cast<QLabel *>(friends->itemWidget(friends->item(0))->findChild<QLabel *>("friendNick"));
        QVERIFY(online);
        QCOMPARE(online->text(), QString("Dave (@davedev)"));
        auto *join = friends->itemWidget(friends->item(0))->findChild<QPushButton *>("joinFriendButton");
        QVERIFY(join);
        QVERIFY(join->isEnabled());                    // Dave онлайн и хостит
        auto *joinOffline = friends->itemWidget(friends->item(1))->findChild<QPushButton *>("joinFriendButton");
        QVERIFY(joinOffline);
        QVERIFY(!joinOffline->isEnabled());            // Eve офлайн — зайти нельзя

        // «зайти к другу» отдаёт его адрес наружу, им занято главное окно
        QSignalSpy spy(&dialog, &FriendsDialog::joinRequested);
        join->click();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().first().toString(), QString("p2p:ABCDEF"));

        // публичный список: чужой мир виден и без дружбы, кнопка Join работает
        auto *hosting = dialog.findChild<QListWidget *>("hostingList");
        QVERIFY(hosting);
        QTRY_COMPARE(hosting->count(), 1);
        auto *hostedJoin = hosting->itemWidget(hosting->item(0))->findChild<QPushButton *>("joinHostedButton");
        QVERIFY(hostedJoin);
        QVERIFY(hostedJoin->isEnabled());
        hostedJoin->click();
        QCOMPARE(spy.count(), 2);
        QCOMPARE(spy.at(1).first().toString(), QString("p2p:HANA01"));

        // входящий запрос: кнопки Accept и Reject, у исходящего — Cancel
        auto *incoming = dialog.findChild<QListWidget *>("incomingRequests");
        auto *outgoing = dialog.findChild<QListWidget *>("outgoingRequests");
        QVERIFY(incoming && outgoing);
        QTRY_COMPARE(incoming->count(), 1);
        QCOMPARE(outgoing->count(), 1);
        QVERIFY(incoming->itemWidget(incoming->item(0))->findChild<QPushButton *>("acceptRequestButton"));
        QVERIFY(incoming->itemWidget(incoming->item(0))->findChild<QPushButton *>("rejectRequestButton"));
        QVERIFY(outgoing->itemWidget(outgoing->item(0))->findChild<QPushButton *>("cancelRequestButton"));

        // поиск по @username: незнакомому игроку предлагается добавить в друзья
        auto *search = dialog.findChild<QLineEdit *>("friendSearchEdit");
        auto *results = dialog.findChild<QListWidget *>("searchResults");
        QVERIFY(search && results);
        search->setText("@bob");
        dialog.findChild<QPushButton *>("friendSearchButton")->click();
        QTRY_COMPARE(results->count(), 2);
        QVERIFY(results->itemWidget(results->item(0))->findChild<QPushButton *>("addFriendButton"));
        auto *tag = results->itemWidget(results->item(1))->findChild<QLabel *>("relationLabel");
        QVERIFY(tag);
        QVERIFY(tag->text().contains("request sent"));

        // отправка запроса уходит на сервер и обновляет список
        const int before = server.requests.count();
        results->itemWidget(results->item(0))->findChild<QPushButton *>("addFriendButton")->click();
        QTRY_VERIFY(server.requests.mid(before).contains("/api/requests"));
        const int sentAt = server.requests.indexOf("/api/requests", before);
        QVERIFY(sentAt >= 0);
        QVERIFY2(server.bodies.at(sentAt).contains("id-bob"), qPrintable(server.bodies.at(sentAt)));

        const QString shot = QDir::tempPath() + "/kogamaz-friends-dialog.png";
        QVERIFY(dialog.grab().save(shot));
    }

    void chatDialogLoadsHistoryAndSends()
    {
        SocialMock server;
        QJsonArray history;
        history.append(QJsonObject{{"id", "m-1"}, {"mine", false}, {"body", "Привет!"},
                                   {"created", 1000}, {"read", false}});
        history.append(QJsonObject{{"id", "m-2"}, {"mine", true}, {"body", "И тебе привет"},
                                   {"created", 2000}, {"read", true}});
        // счётчик для id: сервер выдаёт каждый раз новый, а клиент отбрасывает
        // сообщения с уже известным id — при одном и том же id вторая отправка
        // легитимно считалась бы дубликатом
        int nextId = 3;
        server.route = [&history, &nextId](const QString &path, const QByteArray &body) {
            if (path.startsWith("/api/messages/read")) return SocialMock::json({{"ok", true}});
            if (path.startsWith("/api/messages?peer=")) {
                // страница истории: хвост переписки, hasMore говорит, что старше есть
                return SocialMock::json({{"messages", history}, {"hasMore", true}});
            }
            if (path == "/api/messages") {
                const QJsonObject sent = QJsonDocument::fromJson(body).object();
                const QString id = "m-" + QString::number(nextId++);
                history.append(QJsonObject{{"id", id}, {"mine", true},
                    {"body", sent.value("body").toString()}, {"created", 3000}, {"read", false}});
                return SocialMock::json({{"ok", true}, {"id", id},
                    {"body", sent.value("body").toString()}, {"created", 3000}});
            }
            return QByteArray("{}");
        };

        Settings::setSocialUrl(server.url());
        Settings::setSocialToken("tok-alice");
        Settings::setSocialHandle("alicedev");

        ChatDialog dialog("id-bob", "Bob", "bobdev");
        dialog.show();

        // заголовок показывает, с кем переписываемся, включая @username
        QCOMPARE(dialog.windowTitle(), QString("Chat with Bob (@bobdev)"));

        auto *log = dialog.findChild<QListWidget *>("chatLog");
        QVERIFY(log);
        QTRY_COMPARE(log->count(), 2);                       // история подгрузилась
        auto *mine = dialog.findChild<QListWidget *>("chatLog")->item(1);
        QVERIFY(mine);
        auto *bubble = qobject_cast<QLabel *>(log->itemWidget(mine)->findChild<QLabel *>("chatBubble"));
        QVERIFY(bubble);
        QVERIFY(bubble->text().contains("И тебе привет"));   // текст своего сообщения
        QVERIFY(bubble->text().contains("read"));            // статус прочтения виден

        // входящие помечаются прочитанными, иначе счётчик на сервере не сойдёт
        QTRY_VERIFY(server.requests.contains("/api/messages/read"));

        // догрузка старой истории уходит курсором по created самого старого сообщения
        auto *older = dialog.findChild<QPushButton *>("chatLoadOlderButton");
        QVERIFY(older);
        QVERIFY(older->isVisible());
        older->click();
        QTRY_VERIFY(server.requests.filter("/api/messages?peer=").filter("before=1000").count() > 0);

        // отправка: POST с телом {peer, body}, своё сообщение появляется сразу.
        // Ждём, пока отпустят кнопку: пока в полёте загрузка истории, send()
        // осознанно игнорирует клик, чтобы не склеить два запроса.
        auto *input = dialog.findChild<QLineEdit *>("chatInput");
        auto *send = dialog.findChild<QPushButton *>("chatSendButton");
        QVERIFY(input && send);
        QTRY_VERIFY(send->isEnabled());
        input->setText("Как игра?");
        send->click();
        QTRY_COMPARE(log->count(), 3);                       // ответ сервера добавил пузырь
        const int at = server.requests.indexOf("/api/messages");
        QVERIFY(at >= 0);
        const QJsonObject sentBody = QJsonDocument::fromJson(server.bodies.at(at).toUtf8()).object();
        QCOMPARE(sentBody.value("peer").toString(), QString("id-bob"));
        QCOMPARE(sentBody.value("body").toString(), QString("Как игра?"));
        QVERIFY(input->text().isEmpty());                    // поле очищено после отправки
        QVERIFY(send->isEnabled());                          // и можно писать снова

        // Enter в поле отправляет, а не закрывает диалог
        input->setText("Ещё раз");
        input->returnPressed();
        QTRY_COMPARE(log->count(), 4);

        const QString shot = QDir::tempPath() + "/kogamaz-chat-dialog.png";
        QVERIFY(dialog.grab().save(shot));
    }

    void chatDialogKeepsTextWhenTheServerFails()
    {
        SocialMock server;
        server.route = [](const QString &path, const QByteArray &) {
            if (path.startsWith("/api/messages?peer=")) return SocialMock::json({{"messages", QJsonArray()}, {"hasMore", false}});
            // тело как у реального fail(): сервер шлёт и error, и message
            return SocialMock::json({{"error", "rate_limited"},
                                     {"message", "You are sending messages too fast."}});
        };
        // история грузится, а отправка отклоняется по лимиту
        server.statusCode = [](const QString &path) { return path == "/api/messages" ? 429 : 200; };
        Settings::setSocialUrl(server.url());
        Settings::setSocialToken("tok-alice");

        ChatDialog dialog("id-bob", "Bob", QString());
        dialog.show();
        auto *log = dialog.findChild<QListWidget *>("chatLog");
        QVERIFY(log);
        // путь содержит query-параметры, поэтому ищем по префиксу
        QTRY_VERIFY(!server.requests.filter("/api/messages?peer=").isEmpty());

        auto *input = dialog.findChild<QLineEdit *>("chatInput");
        auto *send = dialog.findChild<QPushButton *>("chatSendButton");
        auto *status = dialog.findChild<QLabel *>("chatStatus");
        QVERIFY(input && send && status);

        input->setText("Сообщение, которое не ушло");
        // ждём, пока отпустят кнопку: пока грузится история, send() не слушает клики
        QTRY_VERIFY(send->isEnabled());
        send->click();

        // ошибка показана, а набранное возвращено в поле — терять текст обидно
        QTRY_VERIFY(status->text().contains("too fast"));
        QCOMPARE(input->text(), QString("Сообщение, которое не ушло"));
        QVERIFY(send->isEnabled());      // и можно повторить попытку
        QCOMPARE(log->count(), 0);       // ничего не подделано локально
    }

    void friendsDialogReadsAndPublishesPosts()
    {
        SocialMock server;
        QJsonArray myPosts;
        myPosts.append(QJsonObject{{"id", "p-1"}, {"body", "Первый пост"}, {"created", 5000}});
        QJsonArray bobPosts;
        bobPosts.append(QJsonObject{{"id", "p-b1"}, {"body", "Пост Боба"}, {"created", 4000}});
        int nextId = 2;
        // Мок различает GET и POST по телу: GET пустой, POST несёт JSON.
        // Без этого на запрос списка приходил бы ответ публикации.
        server.route = [&myPosts, &bobPosts, &nextId](const QString &path, const QByteArray &body) {
            if (path.startsWith("/api/posts?player=id-bob")) return SocialMock::json({{"posts", bobPosts}});
            if (path == "/api/posts") {
                if (body.isEmpty()) return SocialMock::json({{"posts", myPosts}});
                const QJsonObject sent = QJsonDocument::fromJson(body).object();
                const QString id = "p-" + QString::number(nextId++);
                myPosts.append(QJsonObject{{"id", id}, {"body", sent.value("body").toString()}, {"created", 6000}});
                return SocialMock::json({{"ok", true}, {"id", id}, {"body", sent.value("body").toString()}});
            }
            if (path.startsWith("/api/posts/")) return SocialMock::json({{"ok", true}});
            return SocialMock::json({{"friends", QJsonArray()}, {"incoming", QJsonArray()}, {"outgoing", QJsonArray()}});
        };

        Settings::setSocialUrl(server.url());
        Settings::setSocialToken("tok-alice");
        Settings::setSocialHandle("alicedev");
        Settings::setNickname("Alice");

        FriendsDialog dialog;
        dialog.show();

        // свои посты открываются из шапки и показывают поле ввода
        auto *myPostsButton = dialog.findChild<QPushButton *>("myPostsButton");
        auto *stack = dialog.findChild<QStackedWidget *>();
        QVERIFY(myPostsButton && stack);
        myPostsButton->click();
        QCOMPARE(stack->currentIndex(), 2);                    // страница постов открыта
        auto *list = dialog.findChild<QListWidget *>("postsList");
        auto *owner = dialog.findChild<QLabel *>("postsOwner");
        auto *input = dialog.findChild<QLineEdit *>("postInput");
        auto *postButton = dialog.findChild<QPushButton *>("postButton");
        QVERIFY(list && owner && input && postButton);
        QCOMPARE(owner->text(), QString("Your posts"));
        QVERIFY(input->isVisible() && postButton->isVisible());   // свои — можно писать
        QTRY_COMPARE(list->count(), 1);
        auto *body = qobject_cast<QLabel *>(list->itemWidget(list->item(0))->findChild<QLabel *>("postBody"));
        QVERIFY(body);
        QCOMPARE(body->text(), QString("Первый пост"));
        // у своего поста есть кнопка удалить
        QVERIFY(list->itemWidget(list->item(0))->findChild<QPushButton *>("deletePostButton"));

        // публикация: POST с текстом, список перечитывается.
        // Ищем по телу: путь "/api/posts" совпадает и у GET при загрузке,
        // поэтому первое вхождение оказалось бы запросом списка, а не публикацией.
        input->setText("Ещё один пост");
        postButton->click();
        QTRY_COMPARE(list->count(), 2);
        int at = -1;
        for (int i = 0; i < server.bodies.count(); ++i)
            if (server.bodies.at(i).contains("Ещё один пост")) { at = i; break; }
        QVERIFY(at >= 0);
        QVERIFY(server.requests.at(at) == QStringLiteral("/api/posts"));
        QCOMPARE(QJsonDocument::fromJson(server.bodies.at(at).toUtf8()).object()
                     .value("body").toString(), QString("Ещё один пост"));
        QVERIFY(input->text().isEmpty());

        // кнопка «Back» возвращает на список друзей
        dialog.findChild<QPushButton *>("postsBackButton")->click();
        QCOMPARE(stack->currentIndex(), 1);

        // посты друга: поле ввода скрыто, чужой пост удалить нельзя
        dialog.showPosts("id-bob", "Bob", "bobdev");
        QCOMPARE(stack->currentIndex(), 2);
        // заголовок показывает и @username: у разных игроков может быть один ник
        QCOMPARE(owner->text(), QString("Bob (@bobdev)'s posts"));
        QVERIFY(!input->isVisible() && !postButton->isVisible());  // чужой — только чтение
        QTRY_COMPARE(list->count(), 1);
        QCOMPARE(qobject_cast<QLabel *>(list->itemWidget(list->item(0))->findChild<QLabel *>("postBody"))->text(),
                 QString("Пост Боба"));
        QVERIFY(!list->itemWidget(list->item(0))->findChild<QPushButton *>("deletePostButton"));

        const QString shot = QDir::tempPath() + "/kogamaz-posts-page.png";
        QVERIFY(dialog.grab().save(shot));
    }

    void shareBarTogglesBetweenPublicAndByCode()
    {
        ShareBar bar;
        QCOMPARE(Settings::hostPublic(), true);        // публичный по умолчанию

        // переключатель показывает текущий режим и меняет его
        bar.share(7, "My World", "3.5.25");
        auto *toggle = bar.findChild<QPushButton *>("shareVisibilityButton");
        QVERIFY(toggle);
        QCOMPARE(toggle->text(), QString("Public"));
        QSignalSpy spy(&bar, &ShareBar::visibilityChanged);

        toggle->click();
        QCOMPARE(Settings::hostPublic(), false);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().first().toBool(), false);
        QCOMPARE(toggle->text(), QString("By code"));

        toggle->click();
        QCOMPARE(Settings::hostPublic(), true);
        QCOMPARE(spy.count(), 2);
        QCOMPARE(toggle->text(), QString("Public"));

        bar.stop();
        Settings::setHostPublic(true);
    }

    void serverSettingsCheckTheJson()
    {
        ServerConfigDialog dialog;
        auto *edit = dialog.findChild<QPlainTextEdit *>("serverConfigEdit");
        auto *error = dialog.findChild<QLabel *>("serverConfigError");
        auto *save = dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save);
        edit->setPlainText("{\n  \"title\": ");
        save->click();
        QVERIFY(!error->isHidden());
        QVERIFY(error->text().startsWith("Line 2"));
        edit->setPlainText("{ \"httpPort\": 9000 }");
        save->click();
        QVERIFY(error->text().contains("httpPort"));
        edit->setPlainText("{ \"title\": \"Test\", \"clients\": [\"3.5.20\"] }");
        save->click();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(ServerConfigDialog::current().value("title").toString(), QString("Test"));
        ServerConfigDialog reopened;
        const QJsonObject shown = QJsonDocument::fromJson(reopened.findChild<QPlainTextEdit *>("serverConfigEdit")->toPlainText().toUtf8()).object();
        QCOMPARE(shown.value("title").toString(), QString("Test"));
        QVERIFY(shown.contains("public") && !shown.value("public").toBool());
        QVERIFY(QFile::remove(QDir(Paths::serverData()).filePath("server.json")));
    }

    // --- новый UI Этапа 3: логотип, статистика, вкладка профиля --------------

    void brandLogoRendersWordmark()
    {
        const QPixmap logo = BrandLogo::render(QSize(168, 40));
        QCOMPARE(logo.size(), QSize(168, 40));

        // Пустая картинка была бы провалом: логотип обязан быть нарисован
        const QImage image = logo.toImage();
        int painted = 0;
        for (int y = 0; y < image.height(); y += 2)
            for (int x = 0; x < image.width(); x += 2)
                if (qAlpha(image.pixel(x, y)) > 40) ++painted;
        QVERIFY2(painted > 50, "the logo must contain visible pixels");

        // Жёлтая обводка - фирменный цвет тега, ищем её по всей картинке
        bool hasYellow = false;
        for (int y = 0; y < image.height() && !hasYellow; ++y)
            for (int x = 0; x < image.width(); ++x) {
                const QRgb pixel = image.pixel(x, y);
                if (qAlpha(pixel) > 200 && qRed(pixel) > 200 && qGreen(pixel) > 150 && qBlue(pixel) < 90) {
                    hasYellow = true;
                    break;
                }
            }
        QVERIFY2(hasYellow, "the graffiti outline must be yellow");

        // Некорректный размер отдаёт пустую картинку, а не падение
        QVERIFY(BrandLogo::render(QSize(-1, -1)).isNull());
        QCOMPARE(BrandLogo::render(QSize(336, 80)).size(), QSize(336, 80));
    }

    void playerStatsShowsLevelAndProgress()
    {
        PlayerStats stats;
        auto *nick = stats.findChild<QLabel *>("statsNick");
        auto *level = stats.findChild<QLabel *>("statsLevel");
        auto *exp = stats.findChild<QProgressBar *>("statsExp");
        QVERIFY(nick && level && exp);

        // Без входа - состояние «нет аккаунта», а не пустой блок
        stats.refresh();
        QCOMPARE(nick->text(), QString("Not signed in"));
        QVERIFY(exp->value() == 0);

        // 0 опыта = 1 уровень, как на сервере
        stats.apply("FeelZ Dev", "feelz", 0, 1);
        QCOMPARE(nick->text(), QString("FeelZ Dev"));
        QCOMPARE(level->text(), QString("Level 1"));
        QVERIFY(exp->value() == 0);

        // Половина пути до 2 уровня (порог - 100 опыта)
        stats.apply("FeelZ Dev", "feelz", 50, 1);
        QCOMPARE(exp->value(), 50);
        QVERIFY(stats.toolTip().contains("50"));

        // Пустой ник возвращает в состояние «нет аккаунта»
        stats.apply(QString(), QString(), 0, 0);
        QCOMPARE(nick->text(), QString("Not signed in"));
    }

    void profileTabLivesInTheSidebar()
    {
        MainWindow window;
        auto *tabs = window.findChild<QTabWidget *>("mainTabs");
        QVERIFY(tabs);

        // Вкладка обязана быть в списке и попасть в боковое меню
        QWidget *profile = nullptr;
        int profileIndex = -1;
        for (int i = 0; i < tabs->count(); ++i)
            if (qobject_cast<ProfilePage *>(tabs->widget(i))) {
                profile = tabs->widget(i);
                profileIndex = i;
            }
        QVERIFY2(profile, "the Profile tab must exist");
        QCOMPARE(tabs->tabText(profileIndex), QString("Profile"));

        // Кнопка в боковом меню создаётся по всем вкладкам - ищем по тексту
        bool found = false;
        for (QToolButton *button : window.findChildren<QToolButton *>())
            if (button->property("nav").toBool() && button->text() == "Profile") found = true;
        QVERIFY2(found, "the sidebar must show a Profile button");

        // Список постов присутствует вместе с полем ввода и кнопкой публикации.
        // Видимость в offscreen-режиме не проверяем: там она не отражает
        // реального состояния, а состав виджетов - проверяет.
        tabs->setCurrentIndex(profileIndex);
        QCOMPARE(tabs->currentWidget(), profile);
        QVERIFY(profile->findChild<QListWidget *>("profilePosts"));
        QVERIFY(profile->findChild<QLineEdit *>("postInput"));
        QVERIFY(profile->findChild<QPushButton *>("publishPostButton"));
    }

    void headerShowsLogoAndStats()
    {
        MainWindow window;
        auto *logo = window.findChild<QLabel *>("logoLabel");
        QVERIFY(logo);
        // Логотип нарисован кодом и занимает место вордмарка, а не 30x30
        QVERIFY(!logo->pixmap(Qt::ReturnByValue).isNull());
        QCOMPARE(logo->width(), 168);

        // Статистика стоит в шапке справа
        auto *stats = window.findChild<PlayerStats *>("playerStats");
        QVERIFY2(stats, "the header must show the player stats block");
    }

    void versionOrdering()
    {
        // Лексикографический порядок тут обманывает: 1.10.0 новее 1.9.0.
        QCOMPARE(Version::compare("1.9.0", "1.10.0"), -1);
        QCOMPARE(Version::compare("1.10.0", "1.9.0"), 1);
        QCOMPARE(Version::compare("1.2.3", "1.2.3"), 0);

        // Тег релиза подписан с буквой v - она не должна ломать сравнение.
        QCOMPARE(Version::normalize("v1.2.0"), QString("1.2.0"));
        QCOMPARE(Version::normalize("V1.2.0"), QString("1.2.0"));
        QCOMPARE(Version::compare("v1.2.0", "1.2.0"), 0);

        // Метка вроде -beta не участвует: лаунчеру важен основной номер.
        QCOMPARE(Version::normalize("1.2.0-beta"), QString("1.2.0"));
        QCOMPARE(Version::compare("1.2.0-rc1", "1.2.0"), 0);

        // Короткая версия дополняется нулями, а не считается меньше.
        QCOMPARE(Version::compare("1.2", "1.2.0"), 0);

        QVERIFY(Version::isNewer("1.0.1", "1.0.0"));
        QVERIFY(!Version::isNewer("1.0.0", "1.0.1"));
        QVERIFY(!Version::isNewer("1.0.0", "1.0.0"));

        // Пустая текущая версия - сообщаем об обновлении, а не молчим:
        // так безопаснее, чем никогда не показать диалог.
        QVERIFY(Version::isNewer("1.0.0", QString()));
    }

    void parsesGithubRelease()
    {
        // JSON собираем кодом, а не raw-строкой R"(...)": moc (генератор
        // мета-объектов) не умеет в C++ raw-литералы и на них теряет весь
        // класс, даже объявленный выше в файле - сборка падает на линковке
        // с undefined reference to vtable.
        const QJsonObject setupAsset{{"name", "KoGaMaZ-Setup.exe"},
                                     {"browser_download_url", "https://x/setup.exe"}};
        const QJsonObject zipAsset{{"name", "KoGaMaZ.zip"},
                                   {"browser_download_url", "https://x/KoGaMaZ.zip"}};
        const QJsonObject latest{{"tag_name", "v1.4.0"},
                                 {"name", "KoGaMaZ 1.4.0"},
                                 {"html_url", "https://github.com/absolute-spa/kogamaz/releases/tag/v1.4.0"},
                                 {"body", "Added profile posts"},
                                 {"prerelease", false},
                                 {"assets", QJsonArray{setupAsset, zipAsset}}};

        UpdateCheck::Release release = UpdateCheck::parse(QJsonDocument(latest).toJson());
        QVERIFY2(release.isValid(), "a release with a tag must parse");
        QCOMPARE(release.version, QString("1.4.0"));
        QCOMPARE(release.tag, QString("v1.4.0"));
        QCOMPARE(release.title, QString("KoGaMaZ 1.4.0"));
        // Zip берём даже если Setup.exe стоит первым: он нужен самозамене.
        QCOMPARE(release.zipUrl, QString("https://x/KoGaMaZ.zip"));
        QVERIFY(release.htmlUrl.contains("v1.4.0"));

        // Без zip остаётся только ссылка на страницу - ставить вручную.
        const QJsonObject noZip{{"tag_name", "v1.5.0"},
                                {"name", "1.5.0"},
                                {"html_url", "https://x/rel"},
                                {"assets", QJsonArray{QJsonObject{{"name", "Setup.exe"},
                                                                  {"browser_download_url", "https://x/s.exe"}}}}};
        release = UpdateCheck::parse(QJsonDocument(noZip).toJson());
        QVERIFY(release.isValid());
        QVERIFY2(release.zipUrl.isEmpty(), "a non-zip asset must not be treated as portable");

        // Мусор и пустой ответ не должны ронять проверку обновлений.
        QVERIFY(!UpdateCheck::parse("not json at all").isValid());
        QVERIFY(!UpdateCheck::parse(QByteArray()).isValid());
        QVERIFY(!UpdateCheck::parse("[]").isValid());
    }

    void skipsPrereleaseList()
    {
        // /releases отдаёт массив: предрелизы пропускаем, берём первый стабильный.
        const QJsonObject beta{{"tag_name", "v2.0.0-beta"},
                               {"name", "beta"},
                               {"prerelease", true},
                               {"assets", QJsonArray()}};
        const QJsonObject stable{{"tag_name", "v1.6.0"},
                                 {"name", "stable"},
                                 {"prerelease", false},
                                 {"assets", QJsonArray()}};
        const UpdateCheck::Release release =
            UpdateCheck::parse(QJsonDocument(QJsonArray{beta, stable}).toJson());
        QVERIFY(release.isValid());
        QCOMPARE(release.version, QString("1.6.0"));
    }

    void worldMembersDialogShowsRoles()
    {
        // Роли соавторов: мок отвечает списком, диалог обязан его показать и
        // не дать снять автора карты. Здесь важен контракт UI, сервер
        // проверяется в localsocial_test.
        SocialMock server;
        server.route = [](const QString &path, const QByteArray &body) {
            if (path == "/api/worlds/42/members" && body.isEmpty())
                return SocialMock::json({{"world", "42"},
                                         {"myRole", "owner"},
                                         {"members", QJsonArray{
                                             QJsonObject{{"player", "id-me"}, {"nick", "FeelZ"},
                                                         {"handle", "feelz"}, {"role", "owner"},
                                                         {"created", 1.0}, {"self", true}},
                                             QJsonObject{{"player", "id-friend"}, {"nick", "Bob"},
                                                         {"handle", "bob"}, {"role", "member"},
                                                         {"created", 2.0}, {"self", false}}}}});
            if (path == "/api/friends")
                return SocialMock::json({{"friends", QJsonArray{
                                             QJsonObject{{"id", "id-friend"}, {"nick", "Bob"},
                                                         {"handle", "bob"}}}}});
            return SocialMock::json({{"ok", true}});
        };
        Settings::setSocialUrl(server.url());

        WorldMembersDialog dialog(42, QStringLiteral("My map"));
        auto *tree = dialog.findChild<QTreeWidget *>("worldMembersTree");
        auto *friends = dialog.findChild<QComboBox *>("worldMemberFriend");
        auto *roles = dialog.findChild<QComboBox *>("worldMemberRole");
        auto *invite = dialog.findChild<QPushButton *>("worldMemberInvite");
        auto *change = dialog.findChild<QPushButton *>("worldMemberChange");
        auto *remove = dialog.findChild<QPushButton *>("worldMemberRemove");
        QVERIFY(tree && friends && roles && invite && change && remove);

        QTRY_COMPARE(tree->topLevelItemCount(), 2);
        // owner помечен прямо в имени - по нему видно, кто владеет картой
        QCOMPARE(tree->topLevelItem(0)->text(0), QString("FeelZ (owner)"));
        QCOMPARE(tree->topLevelItem(0)->text(2), QString("Owner"));
        QCOMPARE(tree->topLevelItem(1)->text(0), QString("Bob"));
        QCOMPARE(tree->topLevelItem(1)->text(2), QString("Member"));

        // Приглашение только из друзей, и в списке нет роли owner:
        // назначить её нельзя, автор карты и есть owner.
        QTRY_COMPARE(friends->count(), 1);
        QCOMPARE(friends->itemText(0), QString("Bob (bob)"));
        QCOMPARE(friends->itemData(0).toString(), QString("id-friend"));
        QCOMPARE(roles->count(), 3);
        for (int i = 0; i < roles->count(); ++i)
            QVERIFY(roles->itemData(i).toString() != "owner");

        // owner нельзя снять и нельзя сменить роль, member - можно.
        tree->setCurrentItem(tree->topLevelItem(0));
        QVERIFY(!change->isEnabled());
        QVERIFY(!remove->isEnabled());
        QVERIFY(invite->isEnabled());      // а приглашать мне можно
        tree->setCurrentItem(tree->topLevelItem(1));
        QVERIFY(change->isEnabled());
        QVERIFY(remove->isEnabled());

        // Приглашение уходит POST-ом с выбранным другом и ролью по умолчанию.
        // После ответа диалог перечитывает и список, и друзей, поэтому ждём
        // не точное число запросов, а сам POST: он единственный с телом.
        QCOMPARE(friends->currentData().toString(), QString("id-friend"));
        QCOMPARE(roles->currentData().toString(), QString("co-owner"));
        const int before = server.bodies.size();
        invite->click();
        QTRY_VERIFY(server.bodies.size() > before);
        int inviteAt = -1;
        for (int i = before; i < server.bodies.size(); ++i)
            if (!server.bodies.at(i).isEmpty()) { inviteAt = i; break; }
        QVERIFY2(inviteAt >= 0, "the invite must be the request that carried a body");
        QCOMPARE(server.requests.value(inviteAt), QString("/api/worlds/42/members"));
        QVERIFY2(server.bodies.value(inviteAt).contains("co-owner"),
                 "the first role in the list is the default one");

        // Снятие уходит DELETE-ом на конкретного игрока: путь здесь уникален.
        tree->setCurrentItem(tree->topLevelItem(1));
        remove->click();
        QTRY_VERIFY(server.requests.contains("/api/worlds/42/members/id-friend"));

        Settings::setSocialUrl(QString());
    }

    void cleanupTestCase()
    {
        const QString settings = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/settings.ini";
        QFile::remove(settings);
        QDir().rmdir(QFileInfo(settings).absolutePath());
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    const QString package = app.arguments().value(1);
    if (package == "tunnel") {
        QTextStream out(stdout);
        const QStringList args = app.arguments();
        const int rendezvous = args.indexOf("--rendezvous");
        // Мёртвый rendezvous-адрес: туннель молчит и никогда не выдаёт код —
        // ровно то, что происходило на живом VPS без сервиса matching.
        const bool dead = rendezvous >= 0 && args.value(rendezvous + 1).startsWith("10.255.255.1");
        if (args.value(2) == "host") {
            if (dead) { QThread::sleep(120); return 0; }
            out << "code ABCDEF" << Qt::endl << "players 1" << Qt::endl;
            QThread::sleep(5);
            return 0;
        }
        out << "found {\"title\":\"Fake world (Ann)\",\"version\":\"3.5.20\",\"world\":4}" << Qt::endl;
        return 0;
    }
    if (package.startsWith("kogamaPackage:")) {
        QFile capture(QDir::current().filePath("launch-capture.json"));
        if (!capture.open(QIODevice::WriteOnly)) return 1;
        capture.write(QJsonDocument(QJsonObject{{"session", QString::fromUtf8(QByteArray::fromBase64(package.mid(14).toLatin1()))},
            {"cwd", QDir::currentPath()}}).toJson());
        return 0;
    }
    ServersPageTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "serverspage_test.moc"
