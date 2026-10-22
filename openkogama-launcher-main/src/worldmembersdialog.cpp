#include "worldmembersdialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

constexpr int PlayerRole = Qt::UserRole;      // id игрока в колонке 0
constexpr int RoleRole = Qt::UserRole + 1;    // owner | co-owner | tester | member

// Приглашать можно только в эти роли: owner назначить нельзя, автор карты и
// есть owner, и сменить эту роль нельзя никому.
const QStringList InvitableRoles{QStringLiteral("co-owner"),
                                 QStringLiteral("tester"),
                                 QStringLiteral("member")};

// owner = 3, co-owner = 2, tester/member = 1 - как в social-server/server.js.
int roleRank(const QString &role)
{
    if (role == QLatin1String("owner")) return 3;
    if (role == QLatin1String("co-owner")) return 2;
    return 1;
}

bool roleCanManage(const QString &role)
{
    return roleRank(role) >= 2;
}

QString roleText(const QString &role)
{
    if (role == QLatin1String("owner")) return QStringLiteral("Owner");
    if (role == QLatin1String("co-owner")) return QStringLiteral("Co-owner");
    if (role == QLatin1String("tester")) return QStringLiteral("Tester");
    if (role == QLatin1String("member")) return QStringLiteral("Member");
    return role;
}

} // namespace

WorldMembersDialog::WorldMembersDialog(int world, const QString &title, QWidget *parent)
    : QDialog(parent)
    , m_world(world)
{
    setWindowTitle(title.trimmed().isEmpty()
                       ? QStringLiteral("Co-authors")
                       : QStringLiteral("Co-authors of %1").arg(title.trimmed()));
    resize(540, 420);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);

    // Честно о границах роли: иначе игрок решит, что co-owner реально
    // запрещает кому-то строить, и будет ждать защиты, которой нет.
    auto *hint = new QLabel(QStringLiteral(
        "Roles show who works on this map and who may invite others. "
        "They do not block building - the server decides that with its own "
        "join mode."), this);
    hint->setWordWrap(true);
    hint->setObjectName("worldMembersHint");
    layout->addWidget(hint);

    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("worldMembersTree"));
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({QStringLiteral("Player"), QStringLiteral("@username"),
                             QStringLiteral("Role")});
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    layout->addWidget(m_tree, 1);

    // Приглашаем только из списка друзей: случайного человека с улицы звать
    // на карту незачем, а гадать по @username неудобно.
    auto *inviteRow = new QHBoxLayout;
    inviteRow->addWidget(new QLabel(QStringLiteral("Add friend:"), this));
    m_friend = new QComboBox(this);
    m_friend->setObjectName(QStringLiteral("worldMemberFriend"));
    m_friend->setPlaceholderText(QStringLiteral("Nobody to invite"));
    m_role = new QComboBox(this);
    m_role->setObjectName(QStringLiteral("worldMemberRole"));
    for (const QString &role : InvitableRoles) m_role->addItem(roleText(role), role);
    m_invite = new QPushButton(QStringLiteral("Invite"), this);
    m_invite->setObjectName(QStringLiteral("worldMemberInvite"));
    inviteRow->addWidget(m_friend, 1);
    inviteRow->addWidget(m_role);
    inviteRow->addWidget(m_invite);
    layout->addLayout(inviteRow);

    auto *actionRow = new QHBoxLayout;
    m_change = new QPushButton(QStringLiteral("Change role..."), this);
    m_change->setObjectName(QStringLiteral("worldMemberChange"));
    m_remove = new QPushButton(QStringLiteral("Remove"), this);
    m_remove->setObjectName(QStringLiteral("worldMemberRemove"));
    actionRow->addWidget(m_change);
    actionRow->addWidget(m_remove);
    actionRow->addStretch();
    layout->addLayout(actionRow);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("worldMembersStatus"));
    m_status->setWordWrap(true);
    m_status->hide();
    layout->addWidget(m_status);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(m_invite, &QPushButton::clicked, this, &WorldMembersDialog::invite);
    connect(m_change, &QPushButton::clicked, this, &WorldMembersDialog::changeRole);
    connect(m_remove, &QPushButton::clicked, this, &WorldMembersDialog::removeMember);
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this,
            [this]() { updateButtons(); });

    updateButtons();
    reload();
    loadFriends();
}

QTreeWidgetItem *WorldMembersDialog::selectedMember() const
{
    return m_tree->currentItem();
}

void WorldMembersDialog::showProblem(const QString &text)
{
    m_status->setText(text);
    m_status->show();
}

void WorldMembersDialog::updateButtons()
{
    QTreeWidgetItem *item = selectedMember();
    const QString mine = m_myRole;
    const QString theirs = item ? item->data(0, RoleRole).toString() : QString();
    const bool manage = roleCanManage(mine);
    // owner несменяем и неснимаем - это автор карты.
    const bool touchable = item && theirs != QLatin1String("owner");
    m_change->setEnabled(manage && touchable);
    m_remove->setEnabled(manage && touchable
                         && (theirs != QLatin1String("co-owner") || mine == QLatin1String("owner")));
    m_invite->setEnabled(manage && m_friend->count() > 0);
}

void WorldMembersDialog::reload()
{
    Social::worldMembers(m_world, [this](bool ok, const QJsonObject &json, const QString &error) {
        if (!ok) { showProblem(error); return; }
        fill(json);
    });
}

void WorldMembersDialog::fill(const QJsonObject &body)
{
    m_myRole = body.value(QStringLiteral("myRole")).toString();
    const QJsonArray rows = body.value(QStringLiteral("members")).toArray();

    m_tree->clear();
    for (const QJsonValue &value : rows) {
        const QJsonObject row = value.toObject();
        const QString role = row.value(QStringLiteral("role")).toString();
        auto *item = new QTreeWidgetItem(m_tree);
        item->setText(0, row.value(QStringLiteral("nick")).toString());
        item->setText(1, row.value(QStringLiteral("handle")).toString());
        item->setText(2, roleText(role));
        item->setData(0, PlayerRole, row.value(QStringLiteral("player")).toString());
        item->setData(0, RoleRole, role);
        // Автор карты - всегда первым и с пометкой: по нему видно, кто владеет.
        if (role == QLatin1String("owner")) item->setText(0, item->text(0) + QStringLiteral(" (owner)"));
    }
    m_status->hide();
    updateButtons();
}

void WorldMembersDialog::loadFriends()
{
    Social::friends([this](bool ok, const QJsonObject &json, const QString &error) {
        if (!ok) { showProblem(error); return; }

        const QList<Social::Friend> friends = Social::friendListOf(json);
        m_friend->clear();
        for (const Social::Friend &friend_ : friends)
            m_friend->addItem(QStringLiteral("%1 (%2)").arg(friend_.nick, friend_.handle),
                              friend_.id);
        // Выбираем первого вручную: с заданным placeholder Qt оставляет
        // currentIndex = -1, и приглашение ушло бы без игрока.
        if (m_friend->count() > 0) m_friend->setCurrentIndex(0);
        updateButtons();
    });
}

void WorldMembersDialog::invite()
{
    const QString player = m_friend->currentData().toString();
    if (player.isEmpty()) return;
    const QString role = m_role->currentData().toString();
    m_invite->setEnabled(false);

    Social::inviteToWorld(m_world, player, role,
        [this, player, role](bool ok, const QJsonObject &json, const QString &error) {
            Q_UNUSED(json)
            if (!ok) { showProblem(error); updateButtons(); return; }
            Q_UNUSED(player)
            Q_UNUSED(role)
            // Обновляем список и друзей: приглашённый уходит из списка
            // доступных для приглашения.
            reload();
            loadFriends();
        });
}

void WorldMembersDialog::changeRole()
{
    QTreeWidgetItem *item = selectedMember();
    if (!item) return;
    const QString player = item->data(0, PlayerRole).toString();
    const QString current = item->data(0, RoleRole).toString();

    // Выбор из меню, а не общее поле с ролью: поле пришлось бы синхронизировать
    // и с приглашением, и это путало бы, чью роль оно меняет.
    QMenu menu(this);
    for (const QString &role : InvitableRoles) {
        menu.addAction(roleText(role) + (role == current ? QStringLiteral("   (current)") : QString()),
                       this, [this, player, role]() { applyRole(player, role); });
    }
    menu.exec(m_change->mapToGlobal(QPoint(0, m_change->height())));
}

void WorldMembersDialog::applyRole(const QString &player, const QString &role)
{
    Social::inviteToWorld(m_world, player, role,
        [this](bool ok, const QJsonObject &json, const QString &error) {
            Q_UNUSED(json)
            if (!ok) { showProblem(error); return; }
            reload();
        });
}

void WorldMembersDialog::removeMember()
{
    QTreeWidgetItem *item = selectedMember();
    if (!item) return;
    Social::removeWorldMember(m_world, item->data(0, PlayerRole).toString(),
        [this](bool ok, const QJsonObject &json, const QString &error) {
            Q_UNUSED(json)
            if (!ok) { showProblem(error); return; }
            reload();
        });
}
