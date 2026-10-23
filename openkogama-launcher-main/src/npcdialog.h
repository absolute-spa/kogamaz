#pragma once

#include <QDateTime>
#include <QDialog>
#include <QJsonObject>
#include <QPointer>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

// NPC одной карты.
//
// NPC ставят в режиме строительства предметом «NPC» (маленький маркер $$npc,
// на котором плагин держит живого бота с ником). Плагин сервера kogamaz-npc
// записывает, где они стоят, в plugins/data/kogamaz-npc-markers.json, а здесь
// игрок настраивает каждого: ник, поведение, оружие, команду, сложность,
// возрождение и облик (.kgavatar). Настройки уходят в kogamaz-npc-config.json, и плагин
// подхватывает их на лету - даже посреди игры.
class NpcDialog : public QDialog
{
    Q_OBJECT

public:
    NpcDialog(int world, const QString &title, QWidget *parent = nullptr);

    // Файлы обмена с плагином - в папке данных сервера, рядом с его plugins/.
    static QString markersPath();
    static QString configPath();
    static QString openRequestPath();
    // Облики NPC: копии выбранных .kgavatar, плагин берёт их отсюда.
    static QString avatarsPath();

    int world() const { return m_world; }
    // Облик выбранному NPC из файла .kgavatar (кнопка «Load .kgavatar...»).
    // Файл проверяется и копируется в avatarsPath(); применяется по Save.
    bool useAvatar(const QString &path);
    // Выбрать NPC по id маркера. Только что поставленного плагин вносит в
    // список с задержкой до секунды - тогда окно перечитывает его ещё раз.
    void focusMarker(int marker);

private:
    void load();
    void focusMarker(int marker, int tries);
    void retryFocus();
    void fillEditor();
    void storeEditor();
    bool save();
    QTreeWidgetItem *selected() const;
    void refreshRow(QTreeWidgetItem *item);
    void showAvatar();

    int m_world = 0;
    QString m_avatarFile;     // облик выбранного NPC: файл в avatarsPath(), пусто - обычный
    QString m_avatarName;     // имя исходного файла - для показа
    QJsonObject m_config;     // весь файл настроек: правим только свою карту
    QJsonObject m_npcs;       // настройки NPC этой карты по id маркера
    bool m_loading = false;
    int m_focus = 0;          // маркер, который надо выбрать, когда появится
    int m_focusTries = 0;

    QTreeWidget *m_tree = nullptr;
    QLineEdit *m_name = nullptr;
    QComboBox *m_mode = nullptr;
    QComboBox *m_weapon = nullptr;
    QComboBox *m_team = nullptr;
    QComboBox *m_difficulty = nullptr;
    QDoubleSpinBox *m_respawn = nullptr;
    QLabel *m_avatar = nullptr;
    QLabel *m_status = nullptr;
    QWidget *m_editor = nullptr;
};

// Поставили NPC в строительстве - плагин пишет kogamaz-npc-open.json, а
// этот наблюдатель открывает окно NPC поверх игры сразу на нём. Окно одно:
// следующий NPC той же карты выбирается в уже открытом.
class NpcOpener : public QObject
{
    Q_OBJECT

public:
    explicit NpcOpener(QObject *parent);
    ~NpcOpener() override;

    // Проверить файл запроса прямо сейчас (таймер делает это сам).
    void poll();
    NpcDialog *dialog() const { return m_dialog; }

private:
    QTimer *m_timer = nullptr;
    QDateTime m_seen;                 // время последнего обработанного запроса
    QPointer<NpcDialog> m_dialog;
};
