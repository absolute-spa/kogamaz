#include "settingsdialog.h"
#include "ui_settingsdialog.h"
#include "settings.h"
#include "social.h"
#include "theme.h"
#include "discordpresence.h"

#include <QAbstractSocket>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QGroupBox>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHostAddress>
#include <QHostInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr int PingTimeoutMs = 4000;
constexpr int DefaultRendezvousPort = 443;
// Реальный порт сервиса matching — UDP 5058. TCP-пинг по 443/80 показывает только
// задержку до машины, но не факт того, что там запущен P2P-сервис KoGaMaZ.
constexpr int RendezvousProbePort = 5058;
constexpr int ServiceTimeoutMs = 2000;
constexpr int ServerColumn = 0;
constexpr int KindColumn = 1;
constexpr int PingColumn = 2;
constexpr int ServiceColumn = 3;
// Строка 0 таблицы — «Auto»: встроенные P2P-серверы. Она не попадает в список
// адресов, а выбранной может быть всегда — это безопасный вариант по умолчанию.
constexpr int AutoRow = 0;
constexpr int FirstServerRow = 1;

// Хранится в данных ячейки: есть ли на адресе настоящий P2P-сервис.
constexpr int ServiceRole = Qt::UserRole + 1;
enum ServiceState { ServiceUnknown, ServiceYes, ServiceNo };

// Для засеянных пресетов: что это за адрес — рабочий VPS или только ориентир для замера.
QString purposeOf(const QString &server)
{
    static const QHash<QString, QString> Known = {
        {"kogamaz.duckdns.org", QObject::tr("my VPS — hosting")},
        {"139.162.140.8", QObject::tr("Frankfurt — ping only")},
        {"139.162.130.8", QObject::tr("Stockholm — ping only")},
        {"176.58.107.39", QObject::tr("London — ping only")},
        {"45.33.32.156", QObject::tr("Amsterdam — ping only")},
        {"5.101.50.50", QObject::tr("Moscow — ping only")},
        {"45.79.79.174", QObject::tr("Helsinki — ping only")},
        {"176.58.104.20", QObject::tr("Paris — ping only")},
    };
    const QString host = server.trimmed();
    if (Known.contains(host)) return Known.value(host);
    return QObject::tr("custom");
}

QString serviceText(int state)
{
    switch (state) {
    case ServiceYes: return QObject::tr("yes");
    case ServiceNo: return QObject::tr("no");
    default: return QObject::tr("…");
    }
}

// Разбор "host" или "host:port" в пару хост/порт.
bool parseEndpoint(const QString &text, QString *host, quint16 *port)
{
    QString value = text.trimmed();
    if (value.isEmpty()) return false;
    int colon = value.lastIndexOf(':');
    if (colon > 0 && colon < value.size() - 1) {
        bool ok = false;
        const int candidate = value.mid(colon + 1).toInt(&ok);
        if (ok && candidate > 0 && candidate <= 65535) {
            *host = value.left(colon);
            *port = quint16(candidate);
            return !host->isEmpty();
        }
    }
    *host = value;
    *port = DefaultRendezvousPort;
    return true;
}

QString pingText(int ms)
{
    if (ms < 0) return QStringLiteral("—");
    return QString::number(ms) + QStringLiteral(" ms");
}
}

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::SettingsDialog)
{
    ui->setupUi(this);
    ui->nicknameEdit->setText(Settings::nickname());
    ui->consoleOnLaunchCheck->setChecked(Settings::consoleOnLaunch());
    ui->consoleOnCrashCheck->setChecked(Settings::consoleOnCrash());
    ui->discordPresenceCheck->setChecked(Settings::discordPresence());

    buildLanguageGroup();
    buildThemeGroup();
    buildNetworkGroup();
    buildSocialGroup();
    // таблица серверов с пингом требует места, дефолтный размер из .ui тесен
    resize(470, 880);
}

SettingsDialog::~SettingsDialog()
{
    delete ui;
}

void SettingsDialog::buildLanguageGroup()
{
    auto *group = new QGroupBox(tr("Language"), this);
    auto *outer = new QVBoxLayout(group);
    outer->setSpacing(6);

    auto *row = new QHBoxLayout;
    row->setSpacing(8);

    auto *label = new QLabel(tr("Interface language:"), group);

    m_languageCombo = new QComboBox(group);
    m_languageCombo->setObjectName("languageCombo");
    m_languageCombo->setFixedHeight(28);
    // Названия языков пишем на них самих - так принято везде, и «английский»
    // по-английски выглядит бы неправильно. Поэтому эту строку не переводим.
    m_languageCombo->addItem(QStringLiteral("English"), QStringLiteral("en"));
    // QString::fromUtf8, а не QStringLiteral: макрос не имеет членов, и
    // QStringLiteral::fromUtf8 не скомпилируется. Байты записаны в hex
    // ("Русский" в UTF-8), чтобы результат не зависел от того,
    // какую кодировку исходника компилятор решит предположить.
    m_languageCombo->addItem(QString::fromUtf8("\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9"),
                             QStringLiteral("ru"));

    // Пустой код означает английский - он же язык исходников.
    const QString stored = Settings::language();
    const int current = m_languageCombo->findData(
        stored.isEmpty() ? QStringLiteral("en") : stored);
    // currentIndex выставляем явно: с заданным placeholder'ом комбобокс
    // остаётся с индексом -1, и тогда сохранялся бы пустой код языка.
    m_languageCombo->setCurrentIndex(current < 0 ? 0 : current);
    connect(m_languageCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        Settings::setLanguage(m_languageCombo->currentData().toString());
    });

    row->addWidget(label);
    row->addWidget(m_languageCombo);
    row->addStretch(1);
    outer->addLayout(row);

    // Честно предупреждаем: текст задаётся в конструкторах окон, поэтому
    // уже открытые окна остаются на старом языке. Вновь открытые диалоги
    // переведутся сразу.
    auto *hint = new QLabel(
        tr("Applies to the windows you open after restarting the launcher."), group);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: #8b949e;"));
    outer->addWidget(hint);

    ui->verticalLayout->insertWidget(0, group);
}

void SettingsDialog::buildThemeGroup()
{
    auto *group = new QGroupBox(tr("Appearance"), this);
    auto *row = new QHBoxLayout(group);
    row->setSpacing(8);

    auto *label = new QLabel(tr("Accent color:"), group);

    m_accentButton = new QPushButton(group);
    m_accentButton->setObjectName("accentColorButton");
    m_accentButton->setFixedHeight(28);
    m_accentButton->setFixedWidth(90);
    m_accentButton->setToolTip(tr("Pick the launcher accent color"));
    connect(m_accentButton, &QPushButton::clicked, this, &SettingsDialog::pickAccent);

    auto *reset = new QPushButton(tr("Reset"), group);
    reset->setFixedHeight(28);
    connect(reset, &QPushButton::clicked, this, [this]() {
        Settings::setAccentColor("#2f81f7");
        Theme::reapply();
        updateAccentButton();
    });

    row->addWidget(label);
    row->addWidget(m_accentButton);
    row->addWidget(reset);
    row->addStretch(1);

    updateAccentButton();

    const int index = ui->verticalLayout->indexOf(ui->discordGroup) + 1;
    ui->verticalLayout->insertWidget(index, group);
}

void SettingsDialog::updateAccentButton()
{
    if (!m_accentButton) return;
    const QString accent = Settings::accentColor();
    m_accentButton->setText(accent);
    m_accentButton->setStyleSheet(
        QStringLiteral("QPushButton { background: %1; color: #ffffff; border: 1px solid %1; border-radius: 6px; }"
                       "QPushButton:hover { border-color: #ffffff; }").arg(accent));
}

void SettingsDialog::pickAccent()
{
    const QColor chosen = QColorDialog::getColor(QColor(Settings::accentColor()), this, tr("Accent color"));
    if (!chosen.isValid()) return;
    Settings::setAccentColor(chosen.name(QColor::HexRgb));
    Theme::reapply();
    updateAccentButton();
}

void SettingsDialog::buildNetworkGroup()
{
    auto *group = new QGroupBox(tr("P2P server (VPS)"), this);
    auto *layout = new QVBoxLayout(group);
    layout->setSpacing(8);

    auto *hint = new QLabel(tr("The rendezvous server that matches players and relays the tunnel when a "
                               "direct connection fails. Lower ping = snappier hosting.\n"
                               "\"Auto\" uses the built-in P2P servers and is the safe default.\n"
                               "A row only works for hosting if its P2P service column says \"yes\" — "
                               "\"ping only\" rows are datacenter test addresses with nothing running on "
                               "them: pick the closest region there and rent a VPS. Your own VPS row is "
                               "the one you host on once its rendezvous service is deployed."), group);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    m_serverTable = new QTableWidget(group);
    m_serverTable->setObjectName("rendezvousTable");
    m_serverTable->setColumnCount(4);
    m_serverTable->setHorizontalHeaderLabels({tr("Server (host or host:port)"), tr("Purpose"),
                                              tr("Ping"), tr("P2P service")});
    m_serverTable->horizontalHeader()->setSectionResizeMode(ServerColumn, QHeaderView::Stretch);
    m_serverTable->horizontalHeader()->setSectionResizeMode(KindColumn, QHeaderView::ResizeToContents);
    m_serverTable->horizontalHeader()->setSectionResizeMode(PingColumn, QHeaderView::ResizeToContents);
    m_serverTable->horizontalHeader()->setSectionResizeMode(ServiceColumn, QHeaderView::ResizeToContents);
    m_serverTable->verticalHeader()->hide();
    m_serverTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_serverTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_serverTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_serverTable->setShowGrid(false);
    m_serverTable->setMinimumHeight(140);
    layout->addWidget(m_serverTable, 1);

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    auto *add = new QPushButton(tr("Add"), group);
    auto *remove = new QPushButton(tr("Remove"), group);
    auto *refresh = new QPushButton(tr("Measure ping"), group);
    add->setObjectName("addServerButton");
    remove->setObjectName("removeServerButton");
    refresh->setObjectName("measurePingButton");
    add->setFixedHeight(28);
    remove->setFixedHeight(28);
    refresh->setFixedHeight(28);
    connect(add, &QPushButton::clicked, this, &SettingsDialog::addServer);
    connect(remove, &QPushButton::clicked, this, &SettingsDialog::removeServer);
    connect(refresh, &QPushButton::clicked, this, &SettingsDialog::refreshPings);
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addWidget(refresh);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    m_status = new QLabel(group);
    m_status->setObjectName("p2pStatus");
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    connect(m_serverTable, &QTableWidget::itemSelectionChanged, this, &SettingsDialog::updateStatus);

    loadServers();
    updateStatus();
    // пинг и наличие сервиса меряем сразу при открытии — иначе колонки пустые
    QTimer::singleShot(0, this, &SettingsDialog::refreshPings);

    int index = ui->verticalLayout->indexOf(ui->buttonBox);
    if (index < 0) index = ui->verticalLayout->count();
    ui->verticalLayout->insertWidget(index, group);
}

// Группа «Social profile». Ник берётся из общего поля nicknameEdit (вводится один
// раз сверху), здесь только адрес своего сервера и пароль. Нажатие кнопок сразу
// ходит на сервер — так видно, что аккаунт создан/вход выполнен, ещё до OK.
void SettingsDialog::buildSocialGroup()
{
    auto *group = new QGroupBox(tr("Social profile"), this);
    auto *layout = new QVBoxLayout(group);
    layout->setSpacing(8);

    auto *hint = new QLabel(tr("Friends, requests and online status need your own social server. "
                               "Leave the address empty to turn social features off."), group);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto *urlRow = new QHBoxLayout;
    auto *urlLabel = new QLabel(tr("Server:"), group);
    m_socialUrl = new QLineEdit(group);
    m_socialUrl->setObjectName("socialUrlEdit");
    m_socialUrl->setPlaceholderText(tr("https://kogamaz-social.onrender.com"));
    m_socialUrl->setText(Settings::socialUrl());
    m_socialUrl->setClearButtonEnabled(true);
    urlRow->addWidget(urlLabel);
    urlRow->addWidget(m_socialUrl, 1);
    layout->addLayout(urlRow);

    // @username придумывает сам игрок: по нему друзья ищут и добавляют в друзья.
    // Ник (поле сверху) видят все и его можно менять, @username менять нельзя.
    auto *handleRow = new QHBoxLayout;
    auto *handleLabel = new QLabel(tr("Username:"), group);
    m_socialHandle = new QLineEdit(group);
    m_socialHandle->setObjectName("socialHandleEdit");
    m_socialHandle->setPlaceholderText(tr("@yourname"));
    m_socialHandle->setText(Settings::socialHandle());
    m_socialHandle->setMaxLength(20);
    m_socialHandle->setClearButtonEnabled(true);
    handleRow->addWidget(handleLabel);
    handleRow->addWidget(m_socialHandle, 1);
    layout->addLayout(handleRow);

    auto *passRow = new QHBoxLayout;
    auto *passLabel = new QLabel(tr("Password:"), group);
    m_socialPassword = new QLineEdit(group);
    m_socialPassword->setObjectName("socialPasswordEdit");
    m_socialPassword->setEchoMode(QLineEdit::Password);
    m_socialPassword->setPlaceholderText(tr("for the account on this server"));
    m_socialPassword->setClearButtonEnabled(true);
    passRow->addWidget(passLabel);
    passRow->addWidget(m_socialPassword, 1);
    layout->addLayout(passRow);
    auto *buttons = new QHBoxLayout;
    m_socialSignUp = new QPushButton(tr("Create account"), group);
    m_socialSignUp->setObjectName("socialSignUpButton");
    m_socialSignIn = new QPushButton(tr("Sign in"), group);
    m_socialSignIn->setObjectName("socialSignInButton");
    m_socialSignOut = new QPushButton(tr("Sign out"), group);
    m_socialSignOut->setObjectName("socialSignOutButton");
    m_socialSignIn->setDefault(false);
    m_socialSignUp->setDefault(false);
    m_socialSignOut->setDefault(false);
    buttons->addWidget(m_socialSignUp);
    buttons->addWidget(m_socialSignIn);
    buttons->addWidget(m_socialSignOut);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    m_socialStatus = new QLabel(group);
    m_socialStatus->setObjectName("socialStatus");
    m_socialStatus->setWordWrap(true);
    layout->addWidget(m_socialStatus);

    // Всё, что трогает сеть, живёт локально (this), чтобы ответ не пришёл уже в
    // закрытый диалог.
    connect(m_socialUrl, &QLineEdit::editingFinished, this, [this]() {
        Settings::setSocialUrl(m_socialUrl->text());
        updateSocialStatus();
    });

    // Сетевые ответы приходят асинхронно и могут оказаться уже после закрытия
    // диалога, поэтому в каждом callback ловим живость через QPointer, а не
    // захватываем голый this.
    const QPointer<SettingsDialog> self(this);
    const auto busyGuard = [this]() -> bool {
        if (m_busy) return true;
        m_busy = true;
        m_socialSignIn->setEnabled(false);
        m_socialSignUp->setEnabled(false);
        m_socialStatus->setText(tr("Talking to the server..."));
        return false;
    };
    const auto release = [self]() {
        if (!self) return;
        self->m_busy = false;
        self->updateSocialStatus();
    };

    connect(m_socialSignIn, &QPushButton::clicked, this, [this, self, busyGuard, release]() {
        if (busyGuard()) return;
        Settings::setSocialUrl(m_socialUrl->text());
        // Войти можно и по отображаемому нику, и по @username — как кому удобнее.
        const QString nick = ui->nicknameEdit->text().trimmed();
        QString handle = m_socialHandle->text().trimmed().toLower();
        if (handle.startsWith('@')) handle.remove(0, 1);
        if ((nick.isEmpty() && handle.isEmpty()) || m_socialPassword->text().isEmpty()) {
            release();
            m_socialStatus->setText(tr("Enter your nickname (or username) and password first."));
            return;
        }
        if (!nick.isEmpty()) Settings::setNickname(nick);
        const QString who = nick.isEmpty() ? '@' + handle : nick;
        Social::login(who, m_socialPassword->text(), [self, release](bool ok, const QJsonObject &json, const QString &error) {
            if (!self) return;
            if (!ok) {
                release();
                self->m_socialStatus->setText(error);
                return;
            }
            const Social::Account account = Social::accountOf(json);
            Settings::setSocialToken(account.token);
            Settings::setSocialId(account.id);
            if (!account.handle.isEmpty()) Settings::setSocialHandle(account.handle);
            self->m_socialPassword->clear();
            // release() в конце: он дёргает updateSocialStatus(), которому нужен
            // уже сохранённый токен — иначе кнопка «Sign out» останется выключенной
            release();
            self->m_socialStatus->setText(account.handle.isEmpty()
                ? tr("Signed in as %1.").arg(account.nick)
                : tr("Signed in as %1 (@%2).").arg(account.nick, account.handle));
        });
    });

    connect(m_socialSignUp, &QPushButton::clicked, this, [this, self, busyGuard, release]() {
        if (busyGuard()) return;
        Settings::setSocialUrl(m_socialUrl->text());
        const QString nick = ui->nicknameEdit->text().trimmed();
        // @username нормализуем здесь же, чтобы человек увидел итоговую форму
        // до отправки, а серверу не пришлось отвечать отказом на опечатку.
        QString handle = m_socialHandle->text().trimmed().toLower();
        if (handle.startsWith('@')) handle.remove(0, 1);
        if (nick.isEmpty() || handle.isEmpty() || m_socialPassword->text().isEmpty()) {
            release();
            m_socialStatus->setText(tr("Pick a nickname, a username and a password first."));
            return;
        }
        static const QRegularExpression allowed(QStringLiteral("^[a-z0-9_]{3,20}$"));
        if (!allowed.match(handle).hasMatch()) {
            release();
            m_socialStatus->setText(tr("Username may only contain letters, digits and underscores (3-20 characters)."));
            return;
        }
        Settings::setNickname(nick);
        Social::signUp(nick, handle, m_socialPassword->text(),
            [self, release, handle](bool ok, const QJsonObject &json, const QString &error) {
            if (!self) return;
            if (!ok) {
                release();
                self->m_socialStatus->setText(error);
                return;
            }
            const Social::Account account = Social::accountOf(json);
            Settings::setSocialToken(account.token);
            Settings::setSocialId(account.id);
            Settings::setSocialHandle(handle);
            self->m_socialPassword->clear();
            // см. комментарий выше: сначала сохраняем токен, потом обновляем статус
            release();
            self->m_socialStatus->setText(tr("Account created, you are signed in as %1 (@%2).")
                                              .arg(account.nick, handle));
        });
    });

    connect(m_socialSignOut, &QPushButton::clicked, this, [this, self, busyGuard, release]() {
        if (busyGuard()) return;
        const auto clear = [self, release]() {
            Settings::setSocialToken(QString());
            Settings::setSocialId(QString());
            // токен уже удалён к этому моменту, поэтому updateSocialStatus()
            // покажет «Not signed in», а не «Signed in as ...»
            release();
            if (self) self->m_socialStatus->setText(tr("Signed out."));
        };
        if (Settings::socialToken().isEmpty()) {
            clear();
            return;
        }
        // сервер узнаёт аккаунт по токену, поэтому чистим локальное только
        // после успешного ответа, иначе токен останется висеть на сервере
        Social::logout([clear](bool, const QJsonObject &, const QString &) { clear(); });
    });

    updateSocialStatus();

    const int index = ui->verticalLayout->indexOf(ui->buttonBox);
    ui->verticalLayout->insertWidget(index < 0 ? ui->verticalLayout->count() : index, group);
}

void SettingsDialog::updateSocialStatus()
{
    if (!m_socialStatus) return;

    const bool signedIn = !Settings::socialToken().isEmpty();
    m_socialSignOut->setEnabled(signedIn && !m_busy);
    m_socialSignIn->setEnabled(!m_busy);
    m_socialSignUp->setEnabled(!m_busy);

    if (!signedIn) {
        if (Settings::socialUrl().isEmpty())
            m_socialStatus->setText(tr("Social features are off."));
        else
            m_socialStatus->setText(tr("Not signed in."));
        return;
    }

    m_socialStatus->setText(Settings::socialHandle().isEmpty()
        ? tr("Signed in as %1.").arg(Settings::nickname())
        : tr("Signed in as %1 (@%2).").arg(Settings::nickname(), Settings::socialHandle()));
    // сверяем токен с сервером в фоне: он мог истечь или аккаунт могли удалить.
    // Ответ придёт позже, возможно уже после закрытия диалога — проверяем self.
    const QPointer<SettingsDialog> self(this);
    Social::me([self](bool ok, const QJsonObject &json, const QString &) {
        if (!self) return;
        if (!ok) {
            Settings::setSocialToken(QString());
            Settings::setSocialId(QString());
            self->updateSocialStatus();
            return;
        }
        const Social::Account account = Social::accountOf(json);
        Settings::setSocialId(account.id);
        if (!account.handle.isEmpty()) Settings::setSocialHandle(account.handle);
        // self - QPointer, поэтому tr() через него lupdate не разбирает:
        // берём контекст явно, иначе строка не попадёт в файл перевода.
        self->m_socialStatus->setText(account.handle.isEmpty()
            ? QCoreApplication::translate("SettingsDialog", "Signed in as %1.").arg(account.nick)
            : QCoreApplication::translate("SettingsDialog", "Signed in as %1 (@%2).")
                  .arg(account.nick, account.handle));
    });
}

void SettingsDialog::loadServers()
{
    m_activeServer = Settings::rendezvous();
    const QStringList servers = Settings::rendezvousList();
    m_serverTable->setRowCount(servers.size() + FirstServerRow);

    // Строка 0 — «Auto»: встроенные P2P-серверы, всегда работают. Пустой адрес
    // означает "не задавать --rendezvous", поэтому в список она не сохраняется.
    auto *autoItem = new QTableWidgetItem(tr("Auto (built-in P2P)"));
    autoItem->setFlags(autoItem->flags() & ~(Qt::ItemIsEditable | Qt::ItemIsUserCheckable));
    autoItem->setToolTip(tr("Use the built-in KoGaMaZ P2P servers. This is the safe default."));
    m_serverTable->setItem(AutoRow, ServerColumn, autoItem);
    auto *autoKind = new QTableWidgetItem(tr("recommended"));
    autoKind->setFlags(autoKind->flags() & ~Qt::ItemIsEditable);
    m_serverTable->setItem(AutoRow, KindColumn, autoKind);
    m_serverTable->setItem(AutoRow, PingColumn, new QTableWidgetItem(pingText(-1)));
    auto *autoService = new QTableWidgetItem(tr("always"));
    autoService->setFlags(autoService->flags() & ~Qt::ItemIsEditable);
    autoService->setData(ServiceRole, ServiceYes);
    m_serverTable->setItem(AutoRow, ServiceColumn, autoService);

    for (int i = 0; i < servers.size(); ++i) {
        const int row = i + FirstServerRow;
        auto *item = new QTableWidgetItem(servers.at(i));
        m_serverTable->setItem(row, ServerColumn, item);
        auto *kind = new QTableWidgetItem(purposeOf(servers.at(i)));
        kind->setFlags(kind->flags() & ~Qt::ItemIsEditable);
        m_serverTable->setItem(row, KindColumn, kind);
        m_serverTable->setItem(row, PingColumn, new QTableWidgetItem(pingText(-1)));
        m_serverTable->setItem(row, ServiceColumn, new QTableWidgetItem(serviceText(ServiceUnknown)));
    }
    const int active = rowForServer(m_activeServer);
    m_serverTable->selectRow(active >= 0 ? active : AutoRow);
}

void SettingsDialog::saveServers()
{
    QStringList servers;
    // Строка 0 — «Auto», в список адресов она не попадает.
    for (int i = FirstServerRow; i < m_serverTable->rowCount(); ++i) {
        const QString text = m_serverTable->item(i, ServerColumn)->text().trimmed();
        if (!text.isEmpty()) servers << text;
    }
    Settings::setRendezvousList(servers);

    const int row = m_serverTable->currentRow();
    // Auto и пустой выбор = встроенные серверы.
    m_activeServer = (row >= FirstServerRow) ? serverAtRow(row) : QString();
    Settings::setRendezvous(m_activeServer);
}

void SettingsDialog::updateStatus()
{
    if (!m_status) return;
    const int row = m_serverTable->currentRow();
    if (row < FirstServerRow) {
        m_status->setText(tr("Hosting through the built-in KoGaMaZ P2P servers. Recommended — "
                             "these are the only ones guaranteed to work."));
        return;
    }
    const QString server = serverAtRow(row);
    if (server.isEmpty()) {
        m_status->setText(tr("Type a server address, or select Auto to use the built-in servers."));
        return;
    }
    const QTableWidgetItem *cell = m_serverTable->item(row, ServiceColumn);
    switch (cell ? cell->data(ServiceRole).toInt() : ServiceUnknown) {
    case ServiceYes:
        m_status->setText(tr("Hosting through %1 — its rendezvous service answered.").arg(server));
        break;
    case ServiceNo:
        m_status->setText(tr("No KoGaMaZ P2P service answered on %1 — hosting will fall back to "
                             "the built-in servers automatically. To actually host here, rent a VPS "
                             "in this region and deploy the rendezvous service.").arg(server));
        break;
    default:
        m_status->setText(tr("Checking whether %1 runs a KoGaMaZ P2P service...").arg(server));
        break;
    }
}

int SettingsDialog::rowForServer(const QString &server) const
{
    if (server.isEmpty()) return -1;
    for (int i = 0; i < m_serverTable->rowCount(); ++i)
        if (serverAtRow(i) == server) return i;
    return -1;
}

QString SettingsDialog::serverAtRow(int row) const
{
    if (row < 0 || row >= m_serverTable->rowCount()) return {};
    const QTableWidgetItem *item = m_serverTable->item(row, ServerColumn);
    return item ? item->text().trimmed() : QString();
}

void SettingsDialog::addServer()
{
    const int row = m_serverTable->rowCount();
    m_serverTable->insertRow(row);
    m_serverTable->setItem(row, ServerColumn, new QTableWidgetItem(QString()));
    auto *kind = new QTableWidgetItem(tr("custom"));
    kind->setFlags(kind->flags() & ~Qt::ItemIsEditable);
    m_serverTable->setItem(row, KindColumn, kind);
    m_serverTable->setItem(row, PingColumn, new QTableWidgetItem(pingText(-1)));
    m_serverTable->setItem(row, ServiceColumn, new QTableWidgetItem(serviceText(ServiceUnknown)));
    m_serverTable->selectRow(row);
    if (auto *item = m_serverTable->item(row, ServerColumn))
        m_serverTable->editItem(item);
}

void SettingsDialog::removeServer()
{
    const int row = m_serverTable->currentRow();
    // Строка «Auto» не удаляется — это безопасный дефолт.
    if (row < FirstServerRow) return;
    m_serverTable->removeRow(row);
    updateStatus();
}

void SettingsDialog::refreshPings()
{
    for (int i = 0; i < m_serverTable->rowCount(); ++i) {
        const QString server = serverAtRow(i);
        if (auto *cell = m_serverTable->item(i, PingColumn)) cell->setText(pingText(-1));
        if (i == AutoRow || server.isEmpty()) continue;
        if (auto *cell = m_serverTable->item(i, ServiceColumn)) {
            cell->setText(serviceText(ServiceUnknown));
            cell->setData(ServiceRole, ServiceUnknown);
        }
        pingRow(i, server);
        probeService(server);
    }
    updateStatus();
}

void SettingsDialog::setPing(int row, int ms)
{
    if (row < 0 || row >= m_serverTable->rowCount()) return;
    auto *cell = m_serverTable->item(row, PingColumn);
    if (!cell) {
        cell = new QTableWidgetItem;
        m_serverTable->setItem(row, PingColumn, cell);
    }
    cell->setText(pingText(ms));
    QColor color = QColor("#8e98a9");
    if (ms >= 0 && ms < 80) color = QColor("#3fb950");
    else if (ms >= 80 && ms < 200) color = QColor("#d29922");
    else if (ms >= 200) color = QColor("#f85149");
    cell->setForeground(color);
}

void SettingsDialog::pingRow(int row, const QString &server)
{
    QString host;
    quint16 port = 0;
    if (!parseEndpoint(server, &host, &port)) {
        setPing(row, -1);
        return;
    }
    // Если порт не указан явно — пробуем сначала 443, затем 80: у некоторых
    // дата-центров (Amsterdam) открыт только 80, у HTTP-зеркал оба.
    const QList<quint16> ports = server.trimmed().contains(':') ? QList<quint16>{port}
                                                                : QList<quint16>{443, 80};
    probeServer(server, host, ports, 0);
}

void SettingsDialog::probeServer(const QString &server, const QString &host,
                                 const QList<quint16> &ports, int index)
{
    if (index >= ports.size()) { setPingByText(server, -1); return; }

    auto *socket = new QTcpSocket(this);
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    // Один результат на сокет: connected и errorOccurred могут сработать оба,
    // поэтому guard-флаг ставится до любых дальнейших действий.
    auto finish = [this, socket, server, started, host, ports, index](int ms) {
        if (socket->property("done").toBool()) return;
        socket->setProperty("done", true);
        socket->abort();
        socket->deleteLater();
        if (ms >= 0) { setPingByText(server, ms); return; }
        probeServer(server, host, ports, index + 1);   // порт не отвечает — пробуем следующий
    };
    connect(socket, &QTcpSocket::connected, this, [finish, socket, started]() {
        finish(int(QDateTime::currentMSecsSinceEpoch() - started));
    });
    connect(socket, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::errorOccurred),
        this, [finish, socket](QAbstractSocket::SocketError) { finish(-1); });
    // жёсткий таймаут, если хост молчит (без этого соединение могло висеть вечно)
    auto *timeout = new QTimer(socket);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, this, [finish, socket]() { finish(-1); });
    timeout->start(PingTimeoutMs);
    socket->connectToHost(host, ports.at(index));
}

void SettingsDialog::setPingByText(const QString &server, int ms)
{
    // row мог сместиться, пока шёл замер — ищем по тексту адреса, а не по индексу
    for (int i = FirstServerRow; i < m_serverTable->rowCount(); ++i)
        if (serverAtRow(i) == server) { setPing(i, ms); break; }
}

QString SettingsDialog::serviceAtRow(int row) const
{
    const QTableWidgetItem *cell = m_serverTable->item(row, ServiceColumn);
    return cell ? cell->text() : QString();
}

// Проба реального rendezvous-сервиса: он живёт на UDP 5058 и отвечает на любой
// пакет одним байтом. TCP-пинг по 443/80 этого не доказывает — данные-центры
// отвечают на 443, но сервиса там нет, и хост через них не поднимается.
void SettingsDialog::probeService(const QString &server)
{
    const QString text = server.trimmed();
    QString host;
    quint16 declared = 0;
    if (!parseEndpoint(text, &host, &declared)) { setService(server, ServiceNo, serviceText(ServiceNo)); return; }
    // Без явного порта всегда меряем штатный 5058, а не 443 из parseEndpoint.
    const quint16 port = text.contains(':') ? declared : quint16(RendezvousProbePort);

    QHostAddress address;
    if (address.setAddress(host)) { sendServiceProbe(server, address, port); return; }
    // Хост вместо IP: резолвим отдельно, QHostAddress имя не принимает.
    QHostInfo::lookupHost(host, this, [this, server, port](const QHostInfo &info) {
        if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) {
            setService(server, ServiceNo, serviceText(ServiceNo));
            return;
        }
        sendServiceProbe(server, info.addresses().first(), port);
    });
}

void SettingsDialog::sendServiceProbe(const QString &server, const QHostAddress &address, quint16 port)
{
    auto *socket = new QUdpSocket(this);
    auto finish = [this, socket, server](int state) {
        if (socket->property("done").toBool()) return;
        socket->setProperty("done", true);
        socket->close();
        socket->deleteLater();
        setService(server, state, serviceText(state));
    };
    connect(socket, &QUdpSocket::readyRead, this, [finish, socket]() {
        // Любой ответ означает, что на 5058 живёт сервис matching.
        if (socket->hasPendingDatagrams()) finish(ServiceYes);
    });
    auto *timeout = new QTimer(socket);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, this, [finish]() { finish(ServiceNo); });
    timeout->start(ServiceTimeoutMs);
    socket->writeDatagram(QByteArray(12, 0x00), address, port);
}

void SettingsDialog::setService(const QString &server, int state, const QString &label)
{
    // row мог сместиться, пока шла проба — ищем по тексту адреса
    for (int i = FirstServerRow; i < m_serverTable->rowCount(); ++i) {
        if (serverAtRow(i) != server) continue;
        auto *cell = m_serverTable->item(i, ServiceColumn);
        if (!cell) {
            cell = new QTableWidgetItem;
            m_serverTable->setItem(i, ServiceColumn, cell);
        }
        cell->setText(label);
        cell->setData(ServiceRole, state);
        cell->setForeground(state == ServiceYes ? QColor("#3fb950") : QColor("#f85149"));
        break;
    }
    updateStatus();
}

void SettingsDialog::accept()
{
    Settings::setNickname(ui->nicknameEdit->text().trimmed());
    Settings::setConsoleOnLaunch(ui->consoleOnLaunchCheck->isChecked());
    Settings::setConsoleOnCrash(ui->consoleOnCrashCheck->isChecked());
    Settings::setDiscordPresence(ui->discordPresenceCheck->isChecked());
    // пароль мы специально не сохраняем — только адрес сервера; сам вход уже
    // выполнен кнопкой «Sign in» и токен лежит в настройках
    Settings::setSocialUrl(m_socialUrl ? m_socialUrl->text() : Settings::socialUrl());
    saveServers();
    DiscordPresence::refresh();
    QDialog::accept();
}
