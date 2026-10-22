#ifndef ADMINDIALOG_H
#define ADMINDIALOG_H

#include <QDialog>
#include <QUrlQuery>
#include <functional>

class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QPushButton;
class QTreeWidget;

class AdminDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AdminDialog(QWidget *parent = nullptr);

private:
    QWidget *playersPage();
    QWidget *bansPage();
    QWidget *adminsPage();
    void loadPlayers();
    void loadBans();
    void kick();
    void ban();
    void unban();
    void addAdmin();
    void removeAdmin();
    void showAdmins(const QStringList &admins);
    void saveAdmins(const QStringList &admins);
    void post(const QString &path, const QUrlQuery &query, const std::function<void(bool)> &done);
    void updateButtons();
    void showProblem(const QString &text);

    QNetworkAccessManager *m_net;
    QTreeWidget *m_players;
    QTreeWidget *m_bans;
    QTreeWidget *m_admins;
    QLineEdit *m_address;
    QPushButton *m_kick;
    QPushButton *m_ban;
    QPushButton *m_unban;
    QPushButton *m_add;
    QPushButton *m_remove;
    QLabel *m_status;
};

#endif // ADMINDIALOG_H
