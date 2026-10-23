#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <QDialog>

#include <QList>
#include <QtGlobal>

class QComboBox;
class QHostAddress;
class QLabel;
class QPushButton;
class QTableWidget;

namespace Ui {
class SettingsDialog;
}

class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget *parent = nullptr);
    ~SettingsDialog();

    void accept() override;

private:
    // Язык интерфейса. Ставится первой группой: это свойство самого лаунчера,
    // а не отдельной службы, и игроку логично искать его вверху.
    void buildLanguageGroup();
    void buildThemeGroup();
    void buildNetworkGroup();
    // Соц-профиль: адрес своего сервера, пароль аккаунта, вход/регистрация.
    // Ник берётся из общего поля nicknameEdit — вводить его второй раз не нужно.
    void buildSocialGroup();
    // Группы в прокрутку, OK/Cancel внизу - чтобы окно влезало на любой экран.
    void wrapInScrollArea();
    void updateSocialStatus();
    void loadServers();
    void saveServers();
    void addServer();
    void removeServer();
    void refreshPings();
    void pingRow(int row, const QString &server);
    void probeServer(const QString &server, const QString &host, const QList<quint16> &ports, int index);
    void setPing(int row, int ms);
    void setPingByText(const QString &server, int ms);
    // Проверка, есть ли на адресе настоящий P2P-сервис (UDP 5058, rendezvous).
    // Без неё таблица врала бы: адрес с пингом не значит, что через него можно хостить.
    void probeService(const QString &server);
    void sendServiceProbe(const QString &server, const QHostAddress &address, quint16 port);
    void setService(const QString &server, int state, const QString &label);
    void updateStatus();
    void pickAccent();
    void updateAccentButton();
    int rowForServer(const QString &server) const;
    QString serverAtRow(int row) const;
    QString serviceAtRow(int row) const;

    Ui::SettingsDialog *ui;
    QTableWidget *m_serverTable = nullptr;
    QPushButton *m_accentButton = nullptr;
    QComboBox *m_languageCombo = nullptr;
    QLabel *m_status = nullptr;
    QString m_activeServer;
    class QLineEdit *m_socialUrl = nullptr;
    class QLineEdit *m_socialHandle = nullptr;
    class QLineEdit *m_socialPassword = nullptr;
    class QPushButton *m_socialSignIn = nullptr;
    class QPushButton *m_socialSignUp = nullptr;
    class QPushButton *m_socialSignOut = nullptr;
    QLabel *m_socialStatus = nullptr;
    bool m_busy = false;
};

#endif
