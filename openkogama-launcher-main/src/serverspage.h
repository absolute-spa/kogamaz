#pragma once

#include <QWidget>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <functional>

class QAction;
class QEvent;
class QJsonObject;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QNetworkAccessManager;
class QPushButton;
class QShowEvent;
class QTabBar;
class QToolButton;

class ServersPage : public QWidget
{
    Q_OBJECT

public:
    explicit ServersPage(QWidget *parent = nullptr);
    void setCanJoin(bool enabled);
    bool canJoin() const { return m_canJoin; }
    void showFound(const QString &address, const QJsonObject &info);
    void probeThrough(const QString &address, const QString &base, int world = 0);
    bool isPublic(const QString &address) const;
    QString titleOf(const QString &address) const;
    QStringList clientsOf(const QString &address) const;
    void reportPlaying(QObject *game, const QString &address);

signals:
    void joinRequested(const QString &address);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    struct PublicServer
    {
        QString address;
        QString title;
        QStringList clients;
        int players = 0;
        bool thumbnail = false;
    };

    void showView(bool publicView, const QString &select = {});
    void load(const QString &select = {});
    void refresh();
    void fetchPublic();
    void probe(const QString &address);
    void probeCode(const QString &address);
    void get(const QString &url, qint64 limit, std::function<void(int, const QByteArray &)> done);
    void resolve(QStringList candidates, std::function<void(const QString &)> done);
    void keepServer(const QString &address, const QString &name);
    void saveServer();
    void fetchThumbnail(const QString &address, const QString &url);
    void setStatus(const QString &address, const QString &status);
    void updateActions();
    void filter();
    void addServer();
    void joinWithCode();
    void removeServer();
    void copyAddress();
    void showMenu(const QPoint &pos);
    void join();
    bool isSaved(const QString &address) const;
    QListWidgetItem *find(const QString &address) const;
    QString selectedAddress() const;

    QNetworkAccessManager *m_net;
    QTabBar *m_view;
    QListWidget *m_list;
    QLineEdit *m_search;
    QLabel *m_empty;
    QLabel *m_selected;
    QLabel *m_address;
    QToolButton *m_more;
    QAction *m_save;
    QAction *m_remove;
    QPushButton *m_join;
    QPushButton *m_joinCode;
    QSet<QString> m_pending;
    QList<PublicServer> m_public;
    QHash<QString, QByteArray> m_thumbnails;
    bool m_showPublic = true;
    bool m_publicLoaded = false;
    bool m_publicFailed = false;
    bool m_fetching = false;
    bool m_canJoin = false;
};
