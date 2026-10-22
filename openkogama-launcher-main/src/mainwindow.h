#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QStandardItemModel>
#include <QStandardItem>
#include <QModelIndex>
#include <QEvent>
#include <QObject>
#include <functional>
#include <QPointer>
#include <QSet>

class WorldsDialog;
class ShareBar;
class QNetworkAccessManager;
class ServersPage;
class QProcess;
class QToolButton;
class QTimer;
class LocalSocialServer;
class PlayerStats;
class ProfilePage;

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();
    void openLink(const QString &link);

    // Пункт «Проверить обновления» выполняет проверку через этот колбэк.
    // Реализация лежит в updater.cpp, который тянет Velopack и намеренно не
    // входит в тестовые цели: иначе тестам понадобилась бы их DLL, а сами они
    // обновления не проверяют.
    void setUpdateCheck(std::function<void()> check);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onInstallTriggered();
    void onLaunchTriggered();
    void showInstanceMenu(const QPoint &pos);
    void renameInstance();
    void onInstanceRenamed(QStandardItem *item);
    void duplicateInstance();
    void showInstanceFolder();
    void deleteInstance();

private:
    void loadInstances(const QString &selectPath = {});
    void updateInstanceActions();
    void filterInstances();
    void updatePlayPage();
    void onVersionChanged();
    void fetchVersions(std::function<void()> done = nullptr);
    bool launchInstance(const QString &path);
    bool prepareInstance(const QString &path, QString *executable);
    void joinServer(const QString &address);
    void joinCode(const QString &address);
    // Рассылает на соц-сервер «я онлайн и во что играю»; друзья видят это в
    // своём списке. Пустой адрес = просто онлайн, без возможности зайти.
    void announcePresence();

    // Пробует общий соц-сервер по /api/health. Жив - все с этого момента ходят
    // на него (там общие друзья и посты); мёртв - остаёмся на локальном.
    void probeSharedSocial(const QString &sharedUrl, const QString &localUrl);
    void offerVersion(const QStringList &clients, const QString &title, std::function<void()> then);
    void updateHostVersion();
    void editServerSettings();
    void waitForServerToStop(int attempts);
    bool hasRemoteGames() const;
    bool startLocalServer(const QString &version, QProcess **server);
    void syncNav();

    // См. setUpdateCheck(): пусто, пока main.cpp не подключил проверку - в
    // тестах пункт меню молчит, а не падает.
    std::function<void()> m_updateCheck;
    Ui::MainWindow *ui;
    QStandardItemModel *instances;
    QByteArray m_versionsJson;
    QString m_versionsError;

    QPointer<WorldsDialog> m_worlds;
    ShareBar *m_share;
    QNetworkAccessManager *m_net = nullptr;
    ServersPage *m_servers;
    QList<QToolButton *> m_navButtons;
    QList<QPointer<QProcess>> m_remoteClients;
    QPointer<QProcess> m_hostClient;
    QString m_activePath;
    QString m_playingWorld;     // что мы играем прямо сейчас (для друзей)
    QString m_playingAddress;   // адрес, по которому к нам можно присоединиться
    QTimer *m_presenceTimer = nullptr;
    // Встроенный соц-сервер: живёт всё время работы окна, наружу не светится.
    LocalSocialServer *m_socialServer = nullptr;
    // Ник / уровень / опыт в шапке; обновляется после входа и публикаций.
    PlayerStats *m_stats = nullptr;
    // Вкладка «Profile»: аккаунт и посты в боковом меню.
    ProfilePage *m_profile = nullptr;
    bool m_installing = false;
    bool m_localReady = false;
    QSet<QString> m_checkedVersions;
};

#endif // MAINWINDOW_H
