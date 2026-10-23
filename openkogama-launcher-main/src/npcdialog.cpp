#include "npcdialog.h"
#include "paths.h"
#include "theme.h"

#include <QComboBox>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

constexpr int MarkerRole = Qt::UserRole;   // id маркера в колонке 0

// Значения - те же строки, что разбирает плагин (server-plugins/kogamaz-npc).
const QStringList Weapons{QStringLiteral("CenterGun"), QStringLiteral("Shotgun"),
                          QStringLiteral("RailGun"), QStringLiteral("Bazooka"),
                          QStringLiteral("SixShooter"), QStringLiteral("DoubleSixShooter"),
                          QStringLiteral("Flamethrower"), QStringLiteral("ImpulseGun"),
                          QStringLiteral("Sword")};

QJsonObject readJson(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    return doc.isObject() ? doc.object() : QJsonObject();
}

void select(QComboBox *box, const QString &value)
{
    const int index = box->findData(value.toLower(), Qt::UserRole, Qt::MatchFixedString);
    box->setCurrentIndex(index < 0 ? 0 : index);
}

// Сервер пишет .kgavatar в camelCase, а читает без учёта регистра - здесь так же.
QJsonValue field(const QJsonObject &object, const QString &name)
{
    for (auto it = object.begin(); it != object.end(); ++it)
        if (it.key().compare(name, Qt::CaseInsensitive) == 0) return it.value();
    return {};
}

// Аватар ли это: части тела с костью и кубиками в base64 (как AvatarSkin сервера).
bool isAvatar(const QByteArray &data)
{
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    const QJsonArray parts = field(doc.object(), "parts").toArray();
    if (!doc.isObject() || parts.isEmpty()) return false;
    for (const QJsonValue &value : parts) {
        const QJsonObject part = value.toObject();
        const QString cubes = field(part, "cubes").toString();
        if (field(part, "bone").toString().isEmpty() || cubes.isEmpty()
            || !QByteArray::fromBase64Encoding(cubes.toLatin1(), QByteArray::AbortOnBase64DecodingErrors))
            return false;
    }
    return true;
}

} // namespace

QString NpcDialog::markersPath()
{
    return Paths::serverData() + "/plugins/data/kogamaz-npc-markers.json";
}

QString NpcDialog::configPath()
{
    return Paths::serverData() + "/plugins/data/kogamaz-npc-config.json";
}

QString NpcDialog::openRequestPath()
{
    return Paths::serverData() + "/plugins/data/kogamaz-npc-open.json";
}

QString NpcDialog::avatarsPath()
{
    return Paths::serverData() + "/plugins/data/kogamaz-npc-avatars";
}

NpcDialog::NpcDialog(int world, const QString &title, QWidget *parent)
    : QDialog(parent)
    , m_world(world)
{
    setObjectName("npcDialog");
    setWindowTitle(title.trimmed().isEmpty() ? tr("NPC") : tr("NPC of %1").arg(title.trimmed()));
    Theme::fitToScreen(this, {620, 560});

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);

    auto *hint = new QLabel(tr("Place the \"NPC\" item in build mode (Blueprint tab): a bot with a nickname "
                               "appears on it and this window opens by itself. While you build, the bot "
                               "stands still; in a test game and in play it walks, talks or fights. A bot in a "
                               "team the map has (a spawn point of that colour) is a member of it: "
                               "teammates don't attack each other. Changes apply within a second. "
                               "Closed this window? Right-click the NPC cube in build mode, then "
                               "Settings and OK - it opens again."), this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    m_tree = new QTreeWidget(this);
    m_tree->setObjectName("npcTree");
    m_tree->setColumnCount(6);
    m_tree->setHeaderLabels({tr("Nickname"), tr("Behaviour"), tr("Weapon"), tr("Team"),
                             tr("Difficulty"), tr("Position")});
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    layout->addWidget(m_tree, 1);

    m_editor = new QWidget(this);
    auto *form = new QFormLayout(m_editor);
    form->setContentsMargins(0, 0, 0, 0);

    m_name = new QLineEdit(m_editor);
    m_name->setObjectName("npcName");
    m_name->setMaxLength(24);
    m_name->setPlaceholderText(tr("Shown above the head like a player's"));
    form->addRow(tr("Nickname:"), m_name);

    m_mode = new QComboBox(m_editor);
    m_mode->setObjectName("npcMode");
    m_mode->addItem(tr("Attacks players"), "attack");
    m_mode->addItem(tr("Just walks around"), "walk");
    m_mode->addItem(tr("Talks to players"), "talk");
    m_mode->addItem(tr("Stands still (target dummy)"), "idle");
    m_mode->setToolTip(tr("Talks to players: walks around, greets you when you come close and answers in "
                          "the chat when you write next to it or write its nickname. It remembers your "
                          "name and what you like, answers in your language and manner, warms up to "
                          "compliments and takes offence when you shoot it. It doesn't fight.\n"
                          "Stands still: does nothing at all - a target to practise on. Kill it and it "
                          "respawns on its spot."));
    form->addRow(tr("Behaviour:"), m_mode);

    m_weapon = new QComboBox(m_editor);
    m_weapon->setObjectName("npcWeapon");
    m_weapon->addItem(tr("All weapons (switches)"), "all");
    for (const QString &weapon : Weapons) m_weapon->addItem(weapon, weapon.toLower());
    m_weapon->addItem(tr("No weapon"), "none");
    form->addRow(tr("Weapon:"), m_weapon);

    m_team = new QComboBox(m_editor);
    m_team->setObjectName("npcTeam");
    m_team->addItem(tr("On their own"), "none");
    m_team->addItem(tr("Blue"), "blue");
    m_team->addItem(tr("Red"), "red");
    m_team->addItem(tr("Green"), "green");
    m_team->addItem(tr("Yellow"), "yellow");
    form->addRow(tr("Team:"), m_team);

    m_difficulty = new QComboBox(m_editor);
    m_difficulty->setObjectName("npcDifficulty");
    m_difficulty->addItem(tr("Easy"), "easy");
    m_difficulty->addItem(tr("Normal"), "normal");
    m_difficulty->addItem(tr("Hard"), "hard");
    m_difficulty->addItem(tr("Nightmare"), "nightmare");
    m_difficulty->addItem(tr("Insane"), "insane");
    m_difficulty->addItem(tr("Ultra insane"), "ultra");
    m_difficulty->setToolTip(tr("Health and damage are always a player's (100 HP, the weapon's damage). "
                                "Harder bots react faster, aim better, strafe and jump. Nightmare also "
                                "dodges, flanks, shoots back when hit, hunts you down when you hide and "
                                "picks the weapon for the distance. Insane almost never misses, "
                                "bunny-hops, hides behind cover between shots, lies in ambush, stays out "
                                "of your weapon's range and picks a weapon against yours. Ultra insane "
                                "always knows where you are and sneaks up from the side, waits for you "
                                "behind corners, shoots over head-high walls with its body hidden and "
                                "hits from three times farther than your gun reaches."));
    form->addRow(tr("Difficulty:"), m_difficulty);

    m_respawn = new QDoubleSpinBox(m_editor);
    m_respawn->setObjectName("npcRespawn");
    m_respawn->setRange(0, 600);
    m_respawn->setDecimals(0);
    m_respawn->setSuffix(tr(" s"));
    form->addRow(tr("Respawn after:"), m_respawn);

    auto *avatarRow = new QWidget(m_editor);
    auto *avatarLayout = new QHBoxLayout(avatarRow);
    avatarLayout->setContentsMargins(0, 0, 0, 0);
    m_avatar = new QLabel(avatarRow);
    m_avatar->setObjectName("npcAvatar");
    auto *loadAvatar = new QPushButton(tr("Load .kgavatar..."), avatarRow);
    loadAvatar->setObjectName("npcAvatarLoad");
    auto *clearAvatar = new QPushButton(tr("Default"), avatarRow);
    clearAvatar->setObjectName("npcAvatarClear");
    avatarLayout->addWidget(m_avatar, 1);
    avatarLayout->addWidget(loadAvatar);
    avatarLayout->addWidget(clearAvatar);
    form->addRow(tr("Avatar:"), avatarRow);
    connect(loadAvatar, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(this, tr("NPC avatar"), QString(),
                                                          tr("KoGaMa avatars (*.kgavatar *.json)"));
        if (!path.isEmpty()) useAvatar(path);
    });
    connect(clearAvatar, &QPushButton::clicked, this, [this]() {
        m_avatarFile.clear();
        m_avatarName.clear();
        showAvatar();
    });
    layout->addWidget(m_editor);

    m_status = new QLabel(this);
    m_status->setObjectName("npcStatus");
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Save)->setObjectName("npcSave");
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        storeEditor();
        if (save()) m_status->setText(tr("Saved. Bots pick it up within a second."));
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *, QTreeWidgetItem *previous) {
        // переходя на другую строку, не теряем то, что ввели в предыдущей
        if (previous && !m_loading) {
            QTreeWidgetItem *now = m_tree->currentItem();
            m_tree->blockSignals(true);
            m_tree->setCurrentItem(previous);
            storeEditor();
            m_tree->setCurrentItem(now);
            m_tree->blockSignals(false);
        }
        fillEditor();
    });

    load();
}

void NpcDialog::load()
{
    m_loading = true;
    m_config = readJson(configPath());
    const QString key = QString::number(m_world);
    m_npcs = m_config.value("worlds").toObject().value(key).toObject().value("npcs").toObject();

    const QJsonObject markers = readJson(markersPath()).value("worlds").toObject().value(key).toObject();
    const QJsonArray list = markers.value("markers").toArray();
    m_tree->clear();
    for (const QJsonValue &value : list) {
        const QJsonObject marker = value.toObject();
        const QString id = QString::number(marker.value("id").toInt());
        QJsonObject npc = m_npcs.value(id).toObject();
        // ник по умолчанию: подпись маркера ($$npc_Vasya) или «Bot N»
        if (npc.value("name").toString().isEmpty()) {
            const QString label = marker.value("name").toString();
            npc.insert("name", label.isEmpty() ? QStringLiteral("Bot %1").arg(m_tree->topLevelItemCount() + 1) : label);
            m_npcs.insert(id, npc);
        }
        auto *item = new QTreeWidgetItem(m_tree);
        item->setData(0, MarkerRole, id);
        item->setText(5, QStringLiteral("%1, %2, %3")
                             .arg(marker.value("x").toDouble(), 0, 'f', 0)
                             .arg(marker.value("y").toDouble(), 0, 'f', 0)
                             .arg(marker.value("z").toDouble(), 0, 'f', 0));
        refreshRow(item);
    }
    m_loading = false;

    const bool any = m_tree->topLevelItemCount() > 0;
    m_editor->setEnabled(any);
    m_status->setText(any ? QString()
                          : tr("No NPC on this map yet. Open it in build mode, place the \"NPC\" item, "
                               "then open this window again."));
    if (!any) return retryFocus();

    QTreeWidgetItem *found = nullptr;
    if (m_focus != 0) {
        const QString wanted = QString::number(m_focus);
        for (int i = 0; i < m_tree->topLevelItemCount() && !found; ++i)
            if (m_tree->topLevelItem(i)->data(0, MarkerRole).toString() == wanted) found = m_tree->topLevelItem(i);
        if (found) m_focus = 0;
        else retryFocus();
    }
    m_tree->setCurrentItem(found ? found : m_tree->topLevelItem(0));
    if (found) {
        // только что поставленный - сразу можно печатать ник
        m_name->setFocus();
        m_name->selectAll();
    }
}

void NpcDialog::retryFocus()
{
    if (m_focus == 0 || m_focusTries <= 0) {
        m_focus = 0;
        return;
    }
    --m_focusTries;
    QTimer::singleShot(400, this, [this]() {
        if (m_focus != 0) focusMarker(m_focus, m_focusTries);
    });
}

void NpcDialog::focusMarker(int marker)
{
    focusMarker(marker, 10);
}

void NpcDialog::focusMarker(int marker, int tries)
{
    // уже введённое не теряем: сохраняем и перечитываем список
    if (selected()) {
        storeEditor();
        save();
    }
    m_focus = marker;
    m_focusTries = tries;
    load();
}

QTreeWidgetItem *NpcDialog::selected() const
{
    return m_tree->currentItem();
}

void NpcDialog::refreshRow(QTreeWidgetItem *item)
{
    const QJsonObject npc = m_npcs.value(item->data(0, MarkerRole).toString()).toObject();
    const auto textOf = [](QComboBox *box, const QString &value, const QString &fallback) {
        const int index = box->findData(value.toLower(), Qt::UserRole, Qt::MatchFixedString);
        return index < 0 ? box->itemText(box->findData(fallback)) : box->itemText(index);
    };
    item->setText(0, npc.value("name").toString());
    item->setText(1, textOf(m_mode, npc.value("mode").toString(), "attack"));
    item->setText(2, textOf(m_weapon, npc.value("weapon").toString(), "all"));
    item->setText(3, textOf(m_team, npc.value("team").toString(), "none"));
    item->setText(4, textOf(m_difficulty, npc.value("difficulty").toString(), "normal"));
}

void NpcDialog::fillEditor()
{
    QTreeWidgetItem *item = selected();
    m_editor->setEnabled(item != nullptr);
    if (!item) return;
    const QJsonObject npc = m_npcs.value(item->data(0, MarkerRole).toString()).toObject();
    m_name->setText(npc.value("name").toString());
    select(m_mode, npc.value("mode").toString("attack"));
    select(m_weapon, npc.value("weapon").toString("all"));
    select(m_team, npc.value("team").toString("none"));
    select(m_difficulty, npc.value("difficulty").toString("normal"));
    m_respawn->setValue(npc.value("respawn").toDouble(5));
    m_avatarFile = npc.value("avatar").toString();
    m_avatarName = npc.value("avatarName").toString();
    showAvatar();
}

void NpcDialog::showAvatar()
{
    m_avatar->setText(m_avatarFile.isEmpty() ? tr("Default (block boy)")
                      : m_avatarName.isEmpty() ? m_avatarFile
                                               : m_avatarName);
}

bool NpcDialog::useAvatar(const QString &path)
{
    QFile file(path);
    const QByteArray data = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    if (!isAvatar(data)) {
        m_status->setText(tr("This file is not an avatar: %1").arg(QFileInfo(path).fileName()));
        return false;
    }

    // Имя копии - по содержимому: тот же файл второй раз не копируется.
    const QString name = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex().left(12))
                         + ".kgavatar";
    const QString target = avatarsPath() + "/" + name;
    if (!QFile::exists(target)) {
        QDir().mkpath(avatarsPath());
        QSaveFile copy(target);
        if (!copy.open(QIODevice::WriteOnly) || copy.write(data) < 0 || !copy.commit()) {
            m_status->setText(tr("Could not copy the avatar: %1").arg(copy.errorString()));
            return false;
        }
    }
    m_avatarFile = name;
    m_avatarName = QFileInfo(path).completeBaseName();
    showAvatar();
    m_status->setText(tr("Avatar loaded. Press Save to put it on the bot."));
    return true;
}

void NpcDialog::storeEditor()
{
    QTreeWidgetItem *item = selected();
    if (!item) return;
    // Оружие пишем так, как его знает AvatarItem (Shotgun), а не в нижнем
    // регистре из data: так файл легче читать и править руками.
    QString weapon = m_weapon->currentData().toString();
    for (const QString &name : Weapons)
        if (name.compare(weapon, Qt::CaseInsensitive) == 0) weapon = name;

    const QString name = m_name->text().trimmed();
    QJsonObject npc{
        {"name", name.isEmpty() ? item->text(0) : name},
        {"mode", m_mode->currentData().toString()},
        {"weapon", weapon},
        {"team", m_team->currentData().toString()},
        {"difficulty", m_difficulty->currentData().toString()},
        {"respawn", m_respawn->value()},
    };
    if (!m_avatarFile.isEmpty()) {
        npc.insert("avatar", m_avatarFile);
        npc.insert("avatarName", m_avatarName);
    }
    m_npcs.insert(item->data(0, MarkerRole).toString(), npc);
    refreshRow(item);
}

bool NpcDialog::save()
{
    QJsonObject worlds = m_config.value("worlds").toObject();
    QJsonObject mine = worlds.value(QString::number(m_world)).toObject();
    mine.insert("npcs", m_npcs);
    worlds.insert(QString::number(m_world), mine);
    m_config.insert("worlds", worlds);

    QDir().mkpath(QFileInfo(configPath()).absolutePath());
    // Атомарно: плагин читает файл раз в секунду и не должен поймать его
    // наполовину записанным.
    QSaveFile file(configPath());
    if (!file.open(QIODevice::WriteOnly)
        || file.write(QJsonDocument(m_config).toJson(QJsonDocument::Indented)) < 0
        || !file.commit()) {
        m_status->setText(tr("Could not save the NPC settings: %1").arg(file.errorString()));
        return false;
    }
    return true;
}

namespace {

constexpr int OpenPollMs = 500;
constexpr int StaleRequestSecs = 30;   // старый запрос (лаунчер был закрыт) не открываем

QDateTime requestTime(const QJsonObject &request)
{
    return QDateTime::fromString(request.value("at").toString(), Qt::ISODateWithMs);
}

} // namespace

NpcOpener::NpcOpener(QObject *parent)
    : QObject(parent)
{
    // запрос, оставшийся с прошлого запуска, уже не нужен
    m_seen = requestTime(readJson(NpcDialog::openRequestPath()));
    m_timer = new QTimer(this);
    m_timer->setInterval(OpenPollMs);
    connect(m_timer, &QTimer::timeout, this, &NpcOpener::poll);
    m_timer->start();
}

NpcOpener::~NpcOpener()
{
    delete m_dialog.data();   // окно без родителя - убираем сами
}

void NpcOpener::poll()
{
    const QJsonObject request = readJson(NpcDialog::openRequestPath());
    const QDateTime at = requestTime(request);
    if (!at.isValid() || (m_seen.isValid() && at <= m_seen)) return;
    m_seen = at;
    if (at.secsTo(QDateTime::currentDateTimeUtc()) > StaleRequestSecs) return;

    const int world = request.value("world").toInt();
    const int marker = request.value("marker").toInt();
    if (world == 0 || marker == 0) return;

    if (m_dialog && m_dialog->world() != world) m_dialog->close();
    if (!m_dialog) {
        // Без родителя: пока идёт игра, лаунчер обычно свёрнут, а окно с
        // родителем Windows прячет вместе с ним. Поверх всех - иначе его не
        // видно за игрой.
        m_dialog = new NpcDialog(world, request.value("title").toString());
        m_dialog->setAttribute(Qt::WA_DeleteOnClose);
        m_dialog->setWindowFlag(Qt::WindowStaysOnTopHint);
    }
    m_dialog->show();
    m_dialog->raise();
    m_dialog->activateWindow();
    m_dialog->focusMarker(marker);
}
