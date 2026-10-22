#include "profilepage.h"
#include "settings.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

ProfilePage::ProfilePage(QWidget *parent)
    : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    auto *head = new QHBoxLayout;
    m_account = new QLabel(this);
    m_account->setObjectName("profileAccount");
    m_refresh = new QPushButton(tr("Refresh"), this);
    m_refresh->setObjectName("profileRefresh");
    head->addWidget(m_account, 1);
    head->addWidget(m_refresh);
    root->addLayout(head);

    // Поле ввода показывается только для своего аккаунта: чужой профиль
    // остаётся только для чтения.
    auto *compose = new QHBoxLayout;
    compose->setSpacing(6);
    m_input = new QLineEdit(this);
    m_input->setObjectName("postInput");
    m_input->setPlaceholderText(tr("Share something with friends..."));
    m_input->setMaxLength(500);
    m_publish = new QPushButton(tr("Post"), this);
    m_publish->setObjectName("publishPostButton");
    compose->addWidget(m_input, 1);
    compose->addWidget(m_publish);
    root->addLayout(compose);

    m_status = new QLabel(this);
    m_status->setObjectName("postStatus");
    m_status->setWordWrap(true);
    m_status->setStyleSheet(QStringLiteral("color: #8b949e;"));
    root->addWidget(m_status);

    m_posts = new QListWidget(this);
    m_posts->setObjectName("profilePosts");
    m_posts->setSelectionMode(QAbstractItemView::NoSelection);
    m_posts->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    root->addWidget(m_posts, 1);

    connect(m_refresh, &QPushButton::clicked, this, &ProfilePage::reload);
    connect(m_publish, &QPushButton::clicked, this, &ProfilePage::publish);
    connect(m_input, &QLineEdit::returnPressed, this, &ProfilePage::publish);

    reload();
}

void ProfilePage::reload()
{
    loadAccount();
    loadPosts();
}

void ProfilePage::loadAccount()
{
    const bool signedIn = Social::enabled() && !Settings::socialToken().isEmpty();
    m_input->setVisible(signedIn);
    m_publish->setVisible(signedIn);
    m_input->setEnabled(signedIn);
    m_publish->setEnabled(signedIn);

    if (!Social::enabled()) {
        m_account->setText(tr("Social server is not set"));
        m_status->setText(tr("Set the server address in Settings to sign in."));
        return;
    }
    if (!signedIn) {
        m_account->setText(tr("Not signed in"));
        m_status->setText(tr("Register in Settings - it takes a nickname, an @username and a password."));
        return;
    }

    m_account->setText(tr("Signed in as %1 (@%2)")
                           .arg(Settings::nickname(), Settings::socialHandle()));
    m_status->clear();

    // Данные аккаунта приходят отдельно от постов - так ник и уровень в
    // шапке обновятся даже при пустой ленте.
    const QPointer<ProfilePage> self(this);
    Social::me([self](bool ok, const QJsonObject &json, const QString &) {
        if (!self) return;
        if (!ok) return;
        const QString nick = json.value(QLatin1String("nick")).toString();
        const QString handle = json.value(QLatin1String("handle")).toString();
        const int level = json.value(QLatin1String("level")).toInt();
        const qint64 exp = static_cast<qint64>(json.value(QLatin1String("exp")).toDouble());
        // Уровень показываем здесь же: сервер хранит только опыт, а уровень
        // считает из него - игрок должен видеть результат в обоих местах.
        self->m_account->setText(self->tr("Signed in as %1 (@%2) - Level %3, %4 exp")
                                     .arg(nick, handle)
                                     .arg(level)
                                     .arg(exp));
    });
}

void ProfilePage::loadPosts()
{
    const QPointer<ProfilePage> self(this);
    Social::posts(QString(), [self](bool ok, const QJsonObject &json, const QString &error) {
        if (!self || !self->m_posts) return;
        self->m_posts->clear();
        if (!ok) {
            self->m_status->setText(error);
            return;
        }

        const QList<Social::Post> list = Social::postListOf(json);
        if (list.isEmpty()) {
            auto *item = new QListWidgetItem(self->m_posts);
            auto *label = new QLabel(self->tr("No posts yet. Share something above."), self->m_posts);
            label->setWordWrap(true);
            label->setStyleSheet(QStringLiteral("color: #8b949e;"));
            item->setSizeHint(label->sizeHint());
            self->m_posts->setItemWidget(item, label);
            return;
        }

        for (const Social::Post &post : list) {
            auto *item = new QListWidgetItem(self->m_posts);
            item->setData(Qt::UserRole, post.id);

            auto *row = new QWidget(self->m_posts);
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

            // Удалять можно только свои посты - чужой профиль остаётся
            // чужой редакцией, как и на сервере.
            auto *remove = new QPushButton(self->tr("Delete"), row);
            remove->setObjectName("deletePostButton");
            const QString id = post.id;
            QObject::connect(remove, &QPushButton::clicked, self.data(), [self, id]() {
                if (!self) return;
                Social::deletePost(id, [self](bool done, const QJsonObject &, const QString &why) {
                    if (!self) return;
                    if (!done) { self->m_status->setText(why); return; }
                    self->loadPosts();
                });
            });
            foot->addWidget(remove);
            layout->addLayout(foot);

            item->setSizeHint(row->sizeHint());
            self->m_posts->setItemWidget(item, row);
        }
    });
}

void ProfilePage::publish()
{
    const QString text = m_input->text().trimmed();
    if (text.isEmpty() || !m_input->isEnabled()) return;
    m_input->clear();

    const QPointer<ProfilePage> self(this);
    Social::addPost(text, [self, text](bool ok, const QJsonObject &, const QString &error) {
        if (!self) return;
        if (!ok) {
            self->m_status->setText(error);
            self->m_input->setText(text);   // набранное не теряем
            return;
        }
        self->m_status->clear();
        self->loadPosts();
        // Сервер начисляет +10 exp за пост - сообщаем шапке, чтобы уровень
        // и полоса опыта обновились сразу, а не при следующем переключении.
        emit self->statsChanged();
    });
}
