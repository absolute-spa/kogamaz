#pragma once

#include "social.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTimer;

// Личная переписка с одним другом (Этап 2).
//
// История грузится страницами: пока виджет на верхнем краю списка не появится
// кнопка «Load older», догружаем более старые сообщения курсором по created.
// Своя реплика уезжает на сервер сразу, но показывается до ответа — иначе
// при вялом соединении ввод выглядит сломанным.
class ChatDialog : public QDialog
{
    Q_OBJECT

public:
    ChatDialog(const QString &peerId, const QString &peerNick, const QString &peerHandle,
        QWidget *parent = nullptr);

private slots:
    void reload();
    void send();

private:
    void loadHistory(qint64 before);
    void appendMessages(const QList<Social::Message> &messages);
    void prependMessages(const QList<Social::Message> &messages);
    void scrollToBottom();
    void setBusy(bool busy);

    QString m_peerId;
    QString m_peerNick;
    QListWidget *m_log = nullptr;
    QLineEdit *m_input = nullptr;
    QPushButton *m_send = nullptr;
    QPushButton *m_loadOlder = nullptr;
    QLabel *m_status = nullptr;
    QTimer *m_autoRefresh = nullptr;
    bool m_busy = false;
    bool m_loadingOlder = false;
    qint64 m_oldest = 0;      // created самого старого загруженного сообщения
};
