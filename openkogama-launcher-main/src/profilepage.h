#pragma once

#include "social.h"

#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

// Вкладка «Profile» в боковом меню: аккаунт игрока и его посты.
//
// Раньше то же самое жило только в отдельном диалоге «Profile & Friends»,
// из-за чего профиль приходилось открывать через меню File. Теперь это
// обычная вкладка рядом с Versions и Inventory, как в KoGaMa.
class ProfilePage : public QWidget
{
    Q_OBJECT

public:
    explicit ProfilePage(QWidget *parent = nullptr);

    // Перечитывает аккаунт и посты. Вызывается при показе вкладки - так она
    // не врёт после входа, регистрации или публикации в другом месте.
    void reload();

signals:
    // Игрок не вошёл - просим открыть настройки, где есть регистрация.
    void signInRequested();

    // Опыт изменился (например, сервер начислил +10 за пост) - шапке с
    // уровнем надо перечитать аккаунт, иначе уровень отстаёт до
    // переключения вкладки.
    void statsChanged();

private:
    void loadAccount();
    void loadPosts();
    void publish();

    QLabel *m_account = nullptr;
    QLabel *m_status = nullptr;
    QLineEdit *m_input = nullptr;
    QPushButton *m_publish = nullptr;
    QListWidget *m_posts = nullptr;
    QPushButton *m_refresh = nullptr;
};
