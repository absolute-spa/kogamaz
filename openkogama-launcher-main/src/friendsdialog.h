#pragma once

#include "social.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QTimer;

// Profile & friends window: search players by nick, send / accept / reject
// friend requests, see who is online and what they are playing, and join them.
//
// Everything here is read from the social server; the account itself (nick,
// password, server address) is managed in Settings.
class FriendsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit FriendsDialog(QWidget *parent = nullptr);

    // Показывает посты профиля: player пустой = мои, иначе id друга.
    // Публично: так же открывает профиль и кнопка «Posts» у друга.
    void showPosts(const QString &player, const QString &nick, const QString &handle);

signals:
    // Emitted by the "Join" button next to an online friend.
    void joinRequested(const QString &address);
    // Emitted by the "Open Settings" button on the signed-out page.
    void settingsRequested();

private:
    void reload();
    void loadFriends();
    void loadRequests();
    void loadHosting();
    void loadPosts(const QString &player);
    void runSearch();
    void applySignedOutState();
    // Открывает переписку с другом в отдельном не модальном окне.
    void openChat(const QString &peerId, const QString &peerNick, const QString &peerHandle);
    // Публикует пост из поля ввода. Доступно только для своего профиля.
    void publishPost();

    QWidget *friendRow(const Social::Friend &friend_);
    QWidget *requestRow(const Social::Request &request, bool incoming);
    QWidget *hitRow(const Social::SearchHit &hit);
    QWidget *hostedRow(const Social::Hosted &hosted);
    QWidget *postRow(const Social::Post &post);

    QStackedWidget *m_stack = nullptr;
    QLabel *m_accountLine = nullptr;
    QLabel *m_error = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QListWidget *m_results = nullptr;
    QListWidget *m_friends = nullptr;
    QListWidget *m_incoming = nullptr;
    QListWidget *m_outgoing = nullptr;
    QListWidget *m_hosting = nullptr;
    // --- посты профиля ---
    QWidget *m_postsPage = nullptr;
    QListWidget *m_posts = nullptr;
    QLabel *m_postsOwner = nullptr;
    QLineEdit *m_postInput = nullptr;
    QPushButton *m_postButton = nullptr;
    QPushButton *m_postsBack = nullptr;
    QString m_postsPlayer;      // чьи посты смотрим; пусто = свои
    QPushButton *m_refresh = nullptr;
    QTimer *m_autoRefresh = nullptr;
};
