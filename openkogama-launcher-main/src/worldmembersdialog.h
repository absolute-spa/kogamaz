#pragma once

#include "social.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

// Roles of the co-authors of one world.
//
// The roles only bookkeep inside the launcher who invited whom and who may
// invite others. The real access to building is decided by the game server
// with its global join mode; closing it per player would need a .NET plugin
// we do not ship. So this dialog promises "these people work on this map
// together", never "you are not allowed to build".
class WorldMembersDialog : public QDialog
{
    Q_OBJECT

public:
    // world - номер мира из списка Worlds, title - его имя для заголовка.
    WorldMembersDialog(int world, const QString &title, QWidget *parent = nullptr);

private:
    void reload();
    void loadFriends();
    void fill(const QJsonObject &body);
    void invite();
    void changeRole();
    // role: co-owner | tester | member. owner назначить нельзя.
    void applyRole(const QString &player, const QString &role);
    void removeMember();
    void updateButtons();
    void showProblem(const QString &text);
    QTreeWidgetItem *selectedMember() const;

    int m_world = 0;
    QString m_myRole;             // моя роль здесь: решает, что мне можно
    QTreeWidget *m_tree = nullptr;
    QComboBox *m_friend = nullptr;
    QComboBox *m_role = nullptr;
    QPushButton *m_invite = nullptr;
    QPushButton *m_change = nullptr;
    QPushButton *m_remove = nullptr;
    QLabel *m_status = nullptr;
};
