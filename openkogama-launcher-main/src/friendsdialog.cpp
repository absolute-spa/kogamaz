#include "friendsdialog.h"
#include "theme.h"

#include "chatdialog.h"
#include "settings.h"

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QDateTime>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QPointer>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// Каждая строка — свой виджет: у списка нет встроенного способа рисовать
// кнопку внутри элемента.
QWidget *makeRow(QWidget *parent)
{
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(8);
    return row;
}

QHBoxLayout *layoutOf(QWidget *row)
{
    return qobject_cast<QHBoxLayout *>(row->layout());
}

QString relationText(const QString &relation)
{
    if (relation == QLatin1String("friend")) return QObject::tr("already friends");
    if (relation == QLatin1String("outgoing")) return QObject::tr("request sent");
    if (relation == QLatin1String("incoming")) return QObject::tr("wants to be friends");
    if (relation == QLatin1String("self")) return QObject::tr("this is you");
    return QString();
}

} // namespace

FriendsDialog::FriendsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Profile & Friends"));
    Theme::fitToScreen(this, {520, 640});

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(12, 12, 12, 12);
    outer->setSpacing(10);

    auto *header = new QHBoxLayout;
    m_accountLine = new QLabel(this);
    m_accountLine->setObjectName("accountLine");
    header->addWidget(m_accountLine, 1);
    // свои посты открываются отдельной страницей: писать можно только своё
    auto *myPosts = new QPushButton(tr("My posts"), this);
    myPosts->setObjectName("myPostsButton");
    connect(myPosts, &QPushButton::clicked, this, [this]() {
        showPosts(QString(), Settings::nickname(), Settings::socialHandle());
    });
    header->addWidget(myPosts);
    m_refresh = new QPushButton(tr("Refresh"), this);
    m_refresh->setObjectName("refreshButton");
    header->addWidget(m_refresh);
    outer->addLayout(header);

    m_error = new QLabel(this);
    m_error->setObjectName("socialError");
    m_error->setWordWrap(true);
    m_error->setStyleSheet(QStringLiteral("color: #f85149;"));
    outer->addWidget(m_error);

    m_stack = new QStackedWidget(this);
    outer->addWidget(m_stack, 1);

    // --- страница «не входили» -------------------------------------------
    auto *signInPage = new QWidget(m_stack);
    auto *signInLayout = new QVBoxLayout(signInPage);
    signInLayout->addStretch(1);
    auto *signInText = new QLabel(tr("You are not signed in to a social server.\n"
                                     "Open File -> Settings, enter your nickname and password,\n"
                                     "then press \"Create account\" or \"Sign in\"."), signInPage);
    signInText->setObjectName("signedOutHint");
    signInText->setWordWrap(true);
    signInText->setAlignment(Qt::AlignCenter);
    signInLayout->addWidget(signInText);
    auto *openSettings = new QPushButton(tr("Open Settings"), signInPage);
    openSettings->setObjectName("openSettingsButton");
    openSettings->setFixedWidth(160);
    signInLayout->addWidget(openSettings, 0, Qt::AlignHCenter);
    signInLayout->addStretch(1);
    connect(openSettings, &QPushButton::clicked, this, &FriendsDialog::settingsRequested);
    m_stack->addWidget(signInPage);

    // --- страница «всё социальное» ---------------------------------------
    auto *page = new QWidget(m_stack);
    auto *pageLayout = new QVBoxLayout(page);
    pageLayout->setSpacing(10);

    auto *searchLabel = new QLabel(tr("Find players by @username or nickname:"), page);
    pageLayout->addWidget(searchLabel);

    auto *searchRow = new QHBoxLayout;
    m_searchEdit = new QLineEdit(page);
    m_searchEdit->setObjectName("friendSearchEdit");
    m_searchEdit->setPlaceholderText(tr("@username"));
    m_searchEdit->setClearButtonEnabled(true);
    auto *searchButton = new QPushButton(tr("Search"), page);
    searchButton->setObjectName("friendSearchButton");
    searchRow->addWidget(m_searchEdit, 1);
    searchRow->addWidget(searchButton);
    pageLayout->addLayout(searchRow);

    m_results = new QListWidget(page);
    m_results->setObjectName("searchResults");
    m_results->setFixedHeight(130);
    pageLayout->addWidget(m_results);

    auto *requestsLabel = new QLabel(tr("Friend requests:"), page);
    pageLayout->addWidget(requestsLabel);

    m_incoming = new QListWidget(page);
    m_incoming->setObjectName("incomingRequests");
    m_incoming->setFixedHeight(90);
    pageLayout->addWidget(m_incoming);

    auto *outgoingLabel = new QLabel(tr("Sent by you:"), page);
    pageLayout->addWidget(outgoingLabel);

    m_outgoing = new QListWidget(page);
    m_outgoing->setObjectName("outgoingRequests");
    m_outgoing->setFixedHeight(70);
    pageLayout->addWidget(m_outgoing);

    auto *friendsLabel = new QLabel(tr("Your friends:"), page);
    pageLayout->addWidget(friendsLabel);

    m_friends = new QListWidget(page);
    m_friends->setObjectName("friendsList");
    pageLayout->addWidget(m_friends, 1);

    // Публичные миры: у кого сейчас открыт мир для всех. Зайти можно без кода.
    auto *hostingLabel = new QLabel(tr("Open to everyone right now:"), page);
    hostingLabel->setObjectName("hostingLabel");
    pageLayout->addWidget(hostingLabel);

    m_hosting = new QListWidget(page);
    m_hosting->setObjectName("hostingList");
    m_hosting->setFixedHeight(110);
    pageLayout->addWidget(m_hosting);

    m_stack->addWidget(page);

    // --- страница «посты профиля» -----------------------------------------
    // Отдельная страница, а не секция: постов у друга может быть много, и
    // листать их удобнее на весь экран, не ужимая список друзей.
    m_postsPage = new QWidget(m_stack);
    auto *postsLayout = new QVBoxLayout(m_postsPage);
    postsLayout->setSpacing(8);

    auto *postsHeader = new QHBoxLayout;
    m_postsBack = new QPushButton(tr("Back"), m_postsPage);
    m_postsBack->setObjectName("postsBackButton");
    postsHeader->addWidget(m_postsBack);
    m_postsOwner = new QLabel(m_postsPage);
    m_postsOwner->setObjectName("postsOwner");
    m_postsOwner->setStyleSheet(QStringLiteral("font-weight:bold;"));
    postsHeader->addWidget(m_postsOwner, 1);
    postsLayout->addLayout(postsHeader);

    m_posts = new QListWidget(m_postsPage);
    m_posts->setObjectName("postsList");
    postsLayout->addWidget(m_posts, 1);

    // вводим пост только для своего профиля: чужой профиль — только чтение
    auto *composer = new QHBoxLayout;
    m_postInput = new QLineEdit(m_postsPage);
    m_postInput->setObjectName("postInput");
    m_postInput->setPlaceholderText(tr("What's on your mind?"));
    m_postInput->setClearButtonEnabled(true);
    m_postButton = new QPushButton(tr("Post"), m_postsPage);
    m_postButton->setObjectName("postButton");
    composer->addWidget(m_postInput, 1);
    composer->addWidget(m_postButton);
    postsLayout->addLayout(composer);

    m_stack->addWidget(m_postsPage);

    connect(m_postsBack, &QPushButton::clicked, this, [this]() {
        // возврат к списку: сбрасываем контекст, чтобы не осталось чужого профиля
        m_postsPlayer.clear();
        m_stack->setCurrentIndex(1);
        reload();
    });
    connect(m_postButton, &QPushButton::clicked, this, &FriendsDialog::publishPost);
    connect(m_postInput, &QLineEdit::returnPressed, this, &FriendsDialog::publishPost);

    connect(searchButton, &QPushButton::clicked, this, &FriendsDialog::runSearch);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &FriendsDialog::runSearch);
    connect(m_refresh, &QPushButton::clicked, this, &FriendsDialog::reload);

    // список друзей обновляем сами: статус «онлайн» меняется без нашего участия
    m_autoRefresh = new QTimer(this);
    m_autoRefresh->setInterval(15000);
    connect(m_autoRefresh, &QTimer::timeout, this, &FriendsDialog::reload);
    m_autoRefresh->start();

    applySignedOutState();
    reload();
}

void FriendsDialog::applySignedOutState()
{
    const bool signedIn = Social::enabled() && !Settings::socialToken().isEmpty();
    // При входе возвращаемся на список, только если показана страница
    // «не вошли». Без этого автообновление каждые 15 с выбивало бы
    // пользователя с открытой страницы постов обратно к списку друзей.
    if (!signedIn) m_stack->setCurrentIndex(0);
    else if (m_stack->currentIndex() == 0) m_stack->setCurrentIndex(1);
    m_refresh->setEnabled(signedIn);
    if (!signedIn) {
        m_accountLine->setText(Social::enabled() ? tr("Not signed in.") : tr("No social server configured."));
        m_error->clear();
    }
}

// Открывает переписку с другом отдельным не модальным окном: так можно
// держать чат открытым, пока смотришь список друзей или хостишь мир.
// Окно уничтожается само при закрытии, поэтому на него нельзя держать
// длинную ссылку — только QPointer.
void FriendsDialog::openChat(const QString &peerId, const QString &peerNick, const QString &peerHandle)
{
    auto *chat = new ChatDialog(peerId, peerNick, peerHandle, this);
    chat->setAttribute(Qt::WA_DeleteOnClose);
    chat->show();
    chat->raise();
    chat->activateWindow();
}

// Переключает окно на страницу постов профиля. player пустой = свои посты,
// иначе id друга — тогда поле ввода прячется, писать можно только своё.
void FriendsDialog::showPosts(const QString &player, const QString &nick, const QString &handle)
{
    m_postsPlayer = player;
    const bool mine = player.isEmpty();
    m_postsOwner->setText(mine ? tr("Your posts")
                               : tr("%1's posts").arg(handle.isEmpty() ? nick : nick + " (@" + handle + ")"));
    // поле ввода и кнопка нужны только у своего профиля
    m_postInput->setVisible(mine);
    m_postButton->setVisible(mine);
    m_stack->setCurrentIndex(2);
    loadPosts(player);
}

void FriendsDialog::loadPosts(const QString &player)
{
    const QPointer<FriendsDialog> self(this);
    Social::posts(player, [self, player](bool ok, const QJsonObject &json, const QString &error) {
        if (!self || !self->m_posts) return;
        // ответ мог прийти уже для другого профиля — подменять список нечем
        if (self->m_postsPlayer != player) return;
        self->m_posts->clear();
        if (!ok) {
            self->m_error->setText(error);
            return;
        }
        self->m_error->clear();
        const QList<Social::Post> list = Social::postListOf(json);
        if (list.isEmpty()) {
            auto *item = new QListWidgetItem(self->m_posts);
            auto *label = new QLabel(player.isEmpty()
                ? QCoreApplication::translate(
                      "FriendsDialog", "No posts yet. Share something above.")
                : QCoreApplication::translate("FriendsDialog", "No posts yet."),
                self->m_posts);
            label->setWordWrap(true);
            label->setStyleSheet(QStringLiteral("color: #8b949e;"));
            item->setSizeHint(label->sizeHint());
            self->m_posts->setItemWidget(item, label);
            return;
        }
        for (const Social::Post &post : list) {
            auto *item = new QListWidgetItem(self->m_posts);
            item->setData(Qt::UserRole, post.id);
            QWidget *row = self->postRow(post);
            item->setSizeHint(row->sizeHint());
            self->m_posts->setItemWidget(item, row);
        }
    });
}

// Строка поста: текст, время и — для своих — кнопка удалить.
QWidget *FriendsDialog::postRow(const Social::Post &post)
{
    QWidget *row = new QWidget(this);
    auto *layout = new QVBoxLayout(row);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(4);

    auto *text = new QLabel(post.body, row);
    text->setObjectName("postBody");
    text->setWordWrap(true);
    text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(text);

    auto *foot = new QHBoxLayout;
    auto *when = new QLabel(QDateTime::fromMSecsSinceEpoch(post.created)
        .toString(QStringLiteral("dd.MM.yyyy HH:mm")), row);
    when->setObjectName("postTime");
    when->setStyleSheet(QStringLiteral("color: #8b949e;"));
    foot->addWidget(when);
    foot->addStretch(1);

    // удалять можно только свои: чужой профиль остаётся чужой редакцией
    if (m_postsPlayer.isEmpty()) {
        auto *remove = new QPushButton(tr("Delete"), row);
        remove->setObjectName("deletePostButton");
        const QString id = post.id;
        const QPointer<FriendsDialog> self(this);
        connect(remove, &QPushButton::clicked, this, [self, id]() {
            if (!self) return;
            Social::deletePost(id, [self](bool ok, const QJsonObject &, const QString &error) {
                if (!self) return;
                if (!ok) { self->m_error->setText(error); return; }
                self->loadPosts(self->m_postsPlayer);   // перечитываем, чтобы убрать строку
            });
        });
        foot->addWidget(remove);
    }
    layout->addLayout(foot);
    return row;
}

void FriendsDialog::publishPost()
{
    // защищаемся от пустого ввода и от двойного клика, пока запрос в полёте
    const QString text = m_postInput->text().trimmed();
    if (text.isEmpty() || m_postInput->isEnabled() == false) return;
    m_postInput->clear();

    const QPointer<FriendsDialog> self(this);
    Social::addPost(text, [self, text](bool ok, const QJsonObject &, const QString &error) {
        if (!self) return;
        if (!ok) {
            self->m_error->setText(error);
            self->m_postInput->setText(text);   // текст возвращаем, терять набранное обидно
            return;
        }
        self->m_error->clear();
        self->loadPosts(self->m_postsPlayer);
    });
}

void FriendsDialog::reload()
{
    applySignedOutState();
    // На странице постов перечитываем только её: глушить список друзей из-за
    // чужого контекста не нужно, а посты меняться могут (друг удалил свой).
    if (m_stack->currentIndex() == 2) {
        loadPosts(m_postsPlayer);
        return;
    }
    if (m_stack->currentIndex() != 1) return;

    QString account = Settings::socialHandle().isEmpty()
        ? tr("Signed in as %1").arg(Settings::nickname())
        : tr("Signed in as %1 (@%2)").arg(Settings::nickname(), Settings::socialHandle());
    // На встроенном сервере база своя у каждого ПК: друг с другой машины
    // в поиске не найдётся. Говорим об этом прямо, а не молча пустым поиском.
    if (Settings::socialUrl().startsWith("http://127.0.0.1"))
        account += "\n" + tr("Shared server is unreachable: search only covers this PC.");
    m_accountLine->setText(account);
    loadFriends();
    loadRequests();
    loadHosting();
}

// Публичный список: у кого открыт мир для всех. В отличие от списка друзей,
// здесь видны и те, кого вы не добавляли — заходить можно без кода.
void FriendsDialog::loadHosting()
{
    // Ответы приходят асинхронно и могут опоздать уже за закрытие окна,
    // поэтому в каждом callback проверяем, что диалог ещё жив.
    const QPointer<FriendsDialog> self(this);
    Social::hosting([self](bool ok, const QJsonObject &json, const QString &error) {
        if (!self || !self->m_hosting) return;
        self->m_hosting->clear();
        if (!ok) {
            self->m_error->setText(error);
            return;
        }
        const QList<Social::Hosted> list = Social::hostedListOf(json);
        if (list.isEmpty()) {
            auto *item = new QListWidgetItem(self->m_hosting);
            auto *label = new QLabel(QCoreApplication::translate(
        "FriendsDialog", "Nobody is hosting openly right now."), self->m_hosting);
            label->setStyleSheet(QStringLiteral("color: #8b949e;"));
            item->setSizeHint(label->sizeHint());
            self->m_hosting->setItemWidget(item, label);
            return;
        }
        for (const Social::Hosted &hosted : list) {
            auto *item = new QListWidgetItem(self->m_hosting);
            QWidget *row = self->hostedRow(hosted);
            item->setSizeHint(row->sizeHint());
            self->m_hosting->setItemWidget(item, row);
        }
    });
}

QWidget *FriendsDialog::hostedRow(const Social::Hosted &hosted)
{
    QWidget *row = makeRow(this);
    auto *layout = layoutOf(row);

    auto *who = new QLabel(hosted.handle.isEmpty() ? hosted.nick : hosted.nick + " (@" + hosted.handle + ")", row);
    layout->addWidget(who);

    auto *title = new QLabel(hosted.title, row);
    title->setStyleSheet(QStringLiteral("color: #8b949e;"));
    layout->addWidget(title, 1);

    auto *join = new QPushButton(tr("Join"), row);
    join->setObjectName("joinHostedButton");
    const QString address = hosted.address;
    join->setToolTip(tr("Join %1 at %2").arg(hosted.nick, address));
    connect(join, &QPushButton::clicked, this, [this, address]() { emit joinRequested(address); });
    layout->addWidget(join);

    return row;
}

void FriendsDialog::loadFriends()
{
    const QPointer<FriendsDialog> self(this);
    Social::friends([self](bool ok, const QJsonObject &json, const QString &error) {
        if (!self || !self->m_friends) return;
        self->m_friends->clear();
        if (!ok) {
            self->m_error->setText(error);
            return;
        }
        self->m_error->clear();
        const QList<Social::Friend> list = Social::friendListOf(json);
        if (list.isEmpty()) {
            auto *empty = new QListWidgetItem(self->m_friends);
            auto *label = new QLabel(QCoreApplication::translate(
        "FriendsDialog",
        "No friends yet. Search for a nickname above and send a request."),
        self->m_friends);
            label->setWordWrap(true);
            label->setStyleSheet(QStringLiteral("color: #8b949e;"));
            empty->setSizeHint(label->sizeHint());
            self->m_friends->setItemWidget(empty, label);
            return;
        }
        for (const Social::Friend &friend_ : list) {
            auto *item = new QListWidgetItem(self->m_friends);
            QWidget *row = self->friendRow(friend_);
            item->setSizeHint(row->sizeHint());
            self->m_friends->setItemWidget(item, row);
        }
    });
}

void FriendsDialog::loadRequests()
{
    const QPointer<FriendsDialog> self(this);
    Social::requests([self](bool ok, const QJsonObject &json, const QString &error) {
        if (!self || !self->m_incoming || !self->m_outgoing) return;
        self->m_incoming->clear();
        self->m_outgoing->clear();
        if (!ok) {
            self->m_error->setText(error);
            return;
        }
        const QList<Social::Request> incoming = Social::requestListOf(json, QStringLiteral("incoming"));
        const QList<Social::Request> outgoing = Social::requestListOf(json, QStringLiteral("outgoing"));
        for (const Social::Request &request : incoming) {
            auto *item = new QListWidgetItem(self->m_incoming);
            QWidget *row = self->requestRow(request, true);
            item->setSizeHint(row->sizeHint());
            self->m_incoming->setItemWidget(item, row);
        }
        for (const Social::Request &request : outgoing) {
            auto *item = new QListWidgetItem(self->m_outgoing);
            QWidget *row = self->requestRow(request, false);
            item->setSizeHint(row->sizeHint());
            self->m_outgoing->setItemWidget(item, row);
        }
    });
}

void FriendsDialog::runSearch()
{
    const QString term = m_searchEdit->text().trimmed();
    m_results->clear();
    if (term.isEmpty()) return;
    m_error->clear();

    const QPointer<FriendsDialog> self(this);
    Social::search(term, [self](bool ok, const QJsonObject &json, const QString &error) {
        if (!self || !self->m_results) return;
        self->m_results->clear();
        if (!ok) {
            self->m_error->setText(error);
            return;
        }
        const QList<Social::SearchHit> hits = Social::hitListOf(json);
        if (hits.isEmpty()) {
            auto *item = new QListWidgetItem(self->m_results);
            // self - QPointer, поэтому tr() через него lupdate не разбирает:
            // берём контекст явно, иначе строка не попадёт в файл перевода.
            auto *label = new QLabel(QCoreApplication::translate(
                "FriendsDialog", "Nobody with that nickname."), self->m_results);
            label->setStyleSheet(QStringLiteral("color: #8b949e;"));
            item->setSizeHint(label->sizeHint());
            self->m_results->setItemWidget(item, label);
            return;
        }
        for (const Social::SearchHit &hit : hits) {
            auto *item = new QListWidgetItem(self->m_results);
            QWidget *row = self->hitRow(hit);
            item->setSizeHint(row->sizeHint());
            self->m_results->setItemWidget(item, row);
        }
    });
}

QWidget *FriendsDialog::friendRow(const Social::Friend &friend_)
{
    QWidget *row = makeRow(this);
    auto *layout = layoutOf(row);

    auto *dot = new QLabel(row);
    dot->setFixedWidth(12);
    dot->setText(QStringLiteral("●"));
    dot->setStyleSheet(friend_.online ? QStringLiteral("color: #3fb950;")
                                      : QStringLiteral("color: #6e7681;"));
    layout->addWidget(dot);

    auto *nick = new QLabel(friend_.handle.isEmpty() ? friend_.nick
        : friend_.nick + " (@" + friend_.handle + ")", row);
    nick->setObjectName("friendNick");
    layout->addWidget(nick);

    auto *state = new QLabel(row);
    state->setObjectName("friendState");
    if (friend_.online && !friend_.world.isEmpty())
        state->setText(tr("playing %1").arg(friend_.world));
    else if (friend_.online)
        state->setText(tr("online"));
    else
        state->setText(tr("offline"));
    state->setStyleSheet(QStringLiteral("color: #8b949e;"));
    layout->addWidget(state, 1);

    auto *join = new QPushButton(tr("Join"), row);
    join->setObjectName("joinFriendButton");
    join->setEnabled(friend_.online && !friend_.address.isEmpty());
    join->setToolTip(friend_.address.isEmpty() ? tr("They are not hosting right now.")
                                               : tr("Join %1 at %2").arg(friend_.nick, friend_.address));
    const QString address = friend_.address;
    connect(join, &QPushButton::clicked, this, [this, address]() { emit joinRequested(address); });
    layout->addWidget(join);

    // Личная переписка открывается отдельным окном и не зависит от того,
    // онлайн ли друг: история читается и офлайн, отправка уйдёт после связи.
    auto *chat = new QPushButton(tr("Chat"), row);
    chat->setObjectName("chatFriendButton");
    chat->setToolTip(tr("Message %1").arg(friend_.nick));
    const QString peerId = friend_.id;
    const QString peerNick = friend_.nick;
    const QString peerHandle = friend_.handle;
    connect(chat, &QPushButton::clicked, this, [this, peerId, peerNick, peerHandle]() {
        openChat(peerId, peerNick, peerHandle);
    });
    layout->addWidget(chat);

    // Посты друга видны только друзьям — сервер это проверяет сам.
    auto *postsButton = new QPushButton(tr("Posts"), row);
    postsButton->setObjectName("friendPostsButton");
    postsButton->setToolTip(tr("Read %1's posts").arg(friend_.nick));
    connect(postsButton, &QPushButton::clicked, this, [this, peerId, peerNick, peerHandle]() {
        showPosts(peerId, peerNick, peerHandle);
    });
    layout->addWidget(postsButton);

    return row;
}

QWidget *FriendsDialog::requestRow(const Social::Request &request, bool incoming)
{
    QWidget *row = makeRow(this);
    auto *layout = layoutOf(row);

    auto *nick = new QLabel(request.handle.isEmpty() ? request.nick
        : request.nick + " (@" + request.handle + ")", row);
    layout->addWidget(nick, 1);

    if (incoming) {
        auto *accept = new QPushButton(tr("Accept"), row);
        accept->setObjectName("acceptRequestButton");
        auto *reject = new QPushButton(tr("Reject"), row);
        reject->setObjectName("rejectRequestButton");
        const QString id = request.id;
        // Ответ сервера может прийти уже после закрытия окна — ловим self.
        const QPointer<FriendsDialog> self(this);
        connect(accept, &QPushButton::clicked, this, [self, id]() {
            Social::answer(id, QStringLiteral("accept"), [self](bool ok, const QJsonObject &, const QString &error) {
                if (!self) return;
                if (!ok) { self->m_error->setText(error); return; }
                self->reload();
            });
        });
        connect(reject, &QPushButton::clicked, this, [self, id]() {
            Social::answer(id, QStringLiteral("reject"), [self](bool ok, const QJsonObject &, const QString &error) {
                if (!self) return;
                if (!ok) { self->m_error->setText(error); return; }
                self->reload();
            });
        });
        layout->addWidget(accept);
        layout->addWidget(reject);
    } else {
        auto *cancel = new QPushButton(tr("Cancel"), row);
        cancel->setObjectName("cancelRequestButton");
        const QString id = request.id;
        const QPointer<FriendsDialog> self(this);
        connect(cancel, &QPushButton::clicked, this, [self, id]() {
            Social::answer(id, QStringLiteral("cancel"), [self](bool ok, const QJsonObject &, const QString &error) {
                if (!self) return;
                if (!ok) { self->m_error->setText(error); return; }
                self->reload();
            });
        });
        layout->addWidget(cancel);
    }
    return row;
}

QWidget *FriendsDialog::hitRow(const Social::SearchHit &hit)
{
    QWidget *row = makeRow(this);
    auto *layout = layoutOf(row);

    auto *nick = new QLabel(hit.handle.isEmpty() ? hit.nick
        : hit.nick + " (@" + hit.handle + ")", row);
    layout->addWidget(nick, 1);

    if (hit.relation == QLatin1String("self")) {
        auto *tag = new QLabel(relationText(hit.relation), row);
        tag->setStyleSheet(QStringLiteral("color: #8b949e;"));
        layout->addWidget(tag);
        return row;
    }

    if (hit.relation == QLatin1String("none")) {
        auto *add = new QPushButton(tr("Add friend"), row);
        add->setObjectName("addFriendButton");
        const QString target = hit.id;
        const QPointer<FriendsDialog> self(this);
        connect(add, &QPushButton::clicked, this, [self, target, add]() {
            add->setEnabled(false);
            Social::sendRequest(target, [self, add](bool ok, const QJsonObject &, const QString &error) {
                if (!self) return;      // окно могли закрыть, пока ждали ответ
                add->setEnabled(false);
                if (!ok) self->m_error->setText(error);
                else self->m_error->clear();
                self->reload();
            });
        });
        layout->addWidget(add);
        return row;
    }

    // уже друзья / запрос уже отправлен / нам пишут — показываем состояние
    auto *tag = new QLabel(relationText(hit.relation), row);
    tag->setObjectName("relationLabel");
    tag->setStyleSheet(QStringLiteral("color: #8b949e;"));
    layout->addWidget(tag);
    return row;
}
