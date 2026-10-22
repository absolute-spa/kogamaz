#include "playerstats.h"
#include "social.h"
#include "settings.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QProgressBar>
#include <QVBoxLayout>

namespace {

// Повтор квадратичной формулы сервера: нужен только для того, чтобы показать
// прогресс внутри уровня, если сервер уровень не прислал.
int levelOf(qint64 exp)
{
    if (exp <= 0) return 1;
    int level = 1;
    while (exp >= static_cast<qint64>(level) * level * 100) ++level;
    return level;
}

qint64 levelFloor(int level)
{
    return static_cast<qint64>(level - 1) * (level - 1) * 100;
}

qint64 nextLevelFloor(int level)
{
    return static_cast<qint64>(level) * level * 100;
}

} // namespace

PlayerStats::PlayerStats(QWidget *parent)
    : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(10, 6, 10, 6);
    root->setSpacing(2);

    m_nick = new QLabel(tr("Not signed in"), this);
    m_nick->setObjectName("statsNick");

    m_handle = new QLabel(this);
    m_handle->setObjectName("statsHandle");

    auto *head = new QHBoxLayout;
    head->setContentsMargins(0, 0, 0, 0);
    head->setSpacing(6);
    head->addWidget(m_nick, 1);
    head->addWidget(m_handle);

    m_level = new QLabel(this);
    m_level->setObjectName("statsLevel");

    m_exp = new QProgressBar(this);
    m_exp->setObjectName("statsExp");
    m_exp->setRange(0, 100);
    m_exp->setValue(0);
    m_exp->setTextVisible(false);
    m_exp->setFixedHeight(6);

    auto *expRow = new QHBoxLayout;
    expRow->setContentsMargins(0, 0, 0, 0);
    expRow->setSpacing(6);
    expRow->addWidget(m_level);
    expRow->addWidget(m_exp, 1);

    root->addLayout(head);
    root->addLayout(expRow);

    showSignedOut();
}

void PlayerStats::showSignedOut()
{
    m_nick->setText(tr("Not signed in"));
    m_handle->clear();
    m_level->clear();
    m_exp->setValue(0);
    setToolTip(tr("Sign up in Settings to keep friends, posts and stats."));
}

void PlayerStats::apply(const QString &nick, const QString &handle,
                        qint64 exp, int level)
{
    if (nick.isEmpty()) {
        showSignedOut();
        return;
    }
    m_nick->setText(nick);
    m_handle->setText(handle.isEmpty() ? QString() : QLatin1Char('@') + handle);

    if (level < 1) level = levelOf(exp);
    const qint64 floor = levelFloor(level);
    const qint64 ceiling = nextLevelFloor(level);
    const qint64 span = qMax<qint64>(1, ceiling - floor);
    const qint64 gained = (exp - floor) * 100 / span;
    const int percent = static_cast<int>(qBound<qint64>(qint64(0), gained, qint64(100)));

    m_level->setText(tr("Level %1").arg(level));
    m_exp->setValue(percent);
    setToolTip(tr("%1 exp - %2 to level %3")
                   .arg(exp)
                   .arg(ceiling - exp)
                   .arg(level + 1));
}

void PlayerStats::refresh()
{
    if (!Social::enabled() || Settings::socialToken().isEmpty()) {
        showSignedOut();
        return;
    }

    // Ответ может прийти уже после смены аккаунта или закрытия окна,
    // поэтому живость проверяем через QPointer, а не по this.
    const QPointer<PlayerStats> self(this);
    Social::me([self](bool ok, const QJsonObject &json, const QString &) {
        if (!self) return;
        if (!ok) {
            self->showSignedOut();
            return;
        }
        self->apply(json.value(QLatin1String("nick")).toString(),
                    json.value(QLatin1String("handle")).toString(),
                    static_cast<qint64>(json.value(QLatin1String("exp")).toDouble()),
                    json.value(QLatin1String("level")).toInt());
    });
}
