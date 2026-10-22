#pragma once

#include <QWidget>

class QLabel;
class QProgressBar;

// Блок статистики игрока в шапке лаунчера: ник, уровень и опыт до следующего.
//
// Уровень считает сервер по тому же правилу, что и в KoGaMa, поэтому здесь мы
// только показываем то, что вернул /api/me, и не держим собственной копии -
// рассинхрон между клиентами был бы гарантирован.
class PlayerStats : public QWidget
{
    Q_OBJECT

public:
    explicit PlayerStats(QWidget *parent = nullptr);

    // Перечитывает аккаунт с сервера. Безопасно вызывать и без входа:
    // тогда блок переключается в состояние «нет аккаунта».
    void refresh();

    // Быстрое обновление из уже известных данных - после входа, регистрации
    // или публикации поста, чтобы не ждать следующего опроса.
    void apply(const QString &nick, const QString &handle, qint64 exp, int level);

private:
    void showSignedOut();

    QLabel *m_nick = nullptr;
    QLabel *m_handle = nullptr;
    QLabel *m_level = nullptr;
    QProgressBar *m_exp = nullptr;
};
