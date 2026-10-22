#include "mainwindow.h"
#include "admindialog.h"
#include "ui_mainwindow.h"
#include "installdialog.h"
#include "installprogressdialog.h"
#include "launchdialog.h"
#include "assetinstaller.h"
#include "settingsdialog.h"
#include "worldsdialog.h"
#include "serverspage.h"
#include "gamelauncher.h"
#include "discordpresence.h"
#include "links.h"
#include "settings.h"
#include "creditsdialog.h"
#include "serverconfigdialog.h"
#include "sharebar.h"
#include "friendsdialog.h"
#include "social.h"
#include "localsocialserver.h"
#include "brandlogo.h"
#include "playerstats.h"
#include "profilepage.h"
#include "tunnel.h"
#include "paths.h"
#include "theme.h"
#include "webplayerruntime.h"
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileIconProvider>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPixmap>
#include <QStyleHints>
#include <QIcon>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslError>
#include <QMessageBox>
#include <QMenu>
#include <QShortcut>
#include <QDirIterator>
#include <QProgressDialog>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QInputDialog>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QTimer>
#include <QUrl>
#include <QStyle>
#include <QPushButton>
#include <QHBoxLayout>
#include <QButtonGroup>
#include <QToolButton>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QVersionNumber>
#include <QProgressDialog>
#include <algorithm>
#include <memory>
#include <optional>

static constexpr int NameRole = Qt::UserRole + 1;
static constexpr int VersionRole = Qt::UserRole + 2;
static constexpr int UnityRole = Qt::UserRole + 3;

static QString clientVersion(const QStandardItem *item) {
    if (!item) return {};
    const QString version = item->data(VersionRole).toString();
    return version.isEmpty() && item->data(UnityRole).toString().startsWith("3.4") ? "2012" : version;
}

static QString versionTitle(const QString &name, const QString &version) {
    if (version.isEmpty()) return name;
    return name == version ? "KoGaMa " + version : name + " (KoGaMa " + version + ")";
}

static QIcon unknownIcon() {
    QPixmap pixmap(":/unknown.png");
    QPainter painter(&pixmap);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(pixmap.rect(), Theme::dark() ? QColor(Qt::white) : QColor(0x3c, 0x3c, 0x3c));
    return QIcon(pixmap);
}

static QIcon instanceIcon(const QDir &dir, const QFileIconProvider &provider) {
    const QString cached = dir.filePath("icon.png");
    if (QFile::exists(cached)) return QIcon(cached);
    const QString exe = dir.entryList({"*.exe"}, QDir::Files).value(0);
    if (exe.isEmpty()) return unknownIcon();
    const QPixmap pixmap = provider.icon(QFileInfo(dir.filePath(exe))).pixmap(64);
    if (pixmap.isNull()) return unknownIcon();
    pixmap.save(cached);
    return QIcon(pixmap);
}

static bool saveInstanceName(const QString &path, const QString &name) {
    QFile file(QDir(path).filePath("instance.json"));
    QJsonObject meta;
    if (file.open(QIODevice::ReadOnly)) {
        meta = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
    }

    meta["name"] = name;
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    file.write(QJsonDocument(meta).toJson());
    return true;
}

static bool copyInstance(const QString &source, const QString &dest) {
    QDir from(source);
    QDir to(dest);
    if (!to.mkpath(".")) return false;

    QDirIterator it(source, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QFileInfo entry(it.next());
        QString target = to.filePath(from.relativeFilePath(entry.filePath()));
        if (entry.isDir()) {
            if (!to.mkpath(target)) return false;
        } else if (!QFile::copy(entry.filePath(), target)) {
            return false;
        }
    }
    return true;
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setWindowTitle("KoGaMaZ");
    // Граффити-логотип рисуем кодом: файла рисунка в проекте нет.
    ui->logoLabel->setPixmap(BrandLogo::render(ui->logoLabel->size()));
    ui->versionCombo->setPlaceholderText("No versions installed");
    // Таб-бар в форке скрыт (навигация - сайдбар), а corner-виджет табов
    // наследует высоту таб-бара, то есть нулевую: шапка с versionCombo и
    // статистикой схлопывалась в невидимую строку. Оставляем headerBar
    // обычной строкой под логотипом и прижимаем селектор версий вправо.
    ui->headerLayout->insertStretch(0, 1);
    // Статистика игрока справа в той же строке, что и логотип: ник, уровень
    // и полоса опыта - то, что раньше оставалось пустым местом справа сверху.
    m_stats = new PlayerStats(this);
    m_stats->setObjectName("playerStats");
    ui->brandLayout->addStretch(1);
    ui->brandLayout->addWidget(m_stats);
    m_servers = new ServersPage(this);
    ui->mainTabs->insertTab(1, m_servers, tr("Servers"));
    connect(m_servers, &ServersPage::joinRequested, this, &MainWindow::joinServer);
    // Профиль с постами - отдельная вкладка в боковом меню, под Versions,
    // как просили: больше не надо лезть в меню File ради своей ленты.
    m_profile = new ProfilePage(this);
    ui->mainTabs->addTab(m_profile, tr("Profile"));
    connect(m_profile, &ProfilePage::signInRequested, this, [this]() {
        SettingsDialog(this).exec();
        m_stats->refresh();
        m_profile->reload();
    });
    // Сервер начисляет опыт за пост - держим уровень в шапке актуальным.
    connect(m_profile, &ProfilePage::statsChanged, this, [this]() {
        m_stats->refresh();
        m_profile->reload();
    });
    m_share = new ShareBar(this);
    ui->verticalLayout->insertWidget(ui->verticalLayout->indexOf(ui->mainTabs), m_share);
    // Nav sidebar with horizontal labels (TLauncher-style)
    auto *nav = new QWidget;
    auto *navLayout = new QVBoxLayout(nav);
    navLayout->setContentsMargins(6, 4, 0, 4);
    navLayout->setSpacing(2);
    auto *navGroup = new QButtonGroup(this);
    navGroup->setExclusive(true);
    ui->mainTabs->tabBar()->hide();
    for (int i = 0; i < ui->mainTabs->count(); ++i) {
        auto *button = new QToolButton(nav);
        button->setText(ui->mainTabs->tabText(i).remove(QLatin1Char('&')));
        button->setCheckable(true);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setProperty("nav", true);
        navGroup->addButton(button, i);
        navLayout->addWidget(button);
        m_navButtons.append(button);
        connect(button, &QToolButton::clicked, this, [this, i] { ui->mainTabs->setCurrentIndex(i); });
    }
    navLayout->addStretch(1);
    connect(ui->mainTabs, &QTabWidget::currentChanged, this, &MainWindow::syncNav);
    // Показали вкладку профиля - перечитываем её и статистику: за время,
    // пока игрок был на других вкладках, могли прийти посты или смениться аккаунт.
    connect(ui->mainTabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (ui->mainTabs->widget(index) != m_profile) return;
        m_profile->reload();
        m_stats->refresh();
    });
    {
        const int index = ui->verticalLayout->indexOf(ui->mainTabs);
        ui->verticalLayout->removeWidget(ui->mainTabs);
        auto *navRow = new QHBoxLayout;
        navRow->setContentsMargins(0, 0, 0, 0);
        navRow->setSpacing(8);
        navRow->addWidget(nav);
        navRow->addWidget(ui->mainTabs, 1);
        ui->verticalLayout->insertLayout(index, navRow, 1);
    }
    syncNav();
    connect(m_share, &ShareBar::listed, this, [this](const QString &address, const QString &title) {
        if (!m_hostClient || m_hostClient->property("listed").toString() == address) return;
        m_hostClient->setProperty("listed", address);
        DiscordPresence::join(m_hostClient, address, title);
        m_servers->reportPlaying(m_hostClient, address);
    });
    auto *guide = new QAction(tr("Host a server that stays online..."), this);
    connect(guide, &QAction::triggered, this, []() {
        QDesktopServices::openUrl(QUrl("https://github.com/openkogama/openkogama-server/blob/main/HOSTING.md"));
    });
    ui->menuHelp->insertAction(ui->actionCredits, guide);
    // Ручная проверка обновлений. Автопроверка при старте молчит, если ей не
    // удалось ответить, - без этого пункта игрок не может ни повторить проверку,
    // ни увидеть её причину, и жалуется «обновление не работает».
    // Идёт через колбэк - см. MainWindow::setUpdateCheck.
    auto *checkUpdates = new QAction(tr("Check for updates"), this);
    connect(checkUpdates, &QAction::triggered, this, [this]() {
        if (m_updateCheck) m_updateCheck();
    });
    ui->menuHelp->insertAction(ui->actionCredits, checkUpdates);
    auto *serverSettings = new QAction(tr("Server settings..."), this);
    connect(serverSettings, &QAction::triggered, this, &MainWindow::editServerSettings);
    ui->menuFile->addAction(serverSettings);
    auto *admin = new QAction(tr("Admin..."), this);
    connect(admin, &QAction::triggered, this, [this]() { AdminDialog(this).exec(); });
    ui->menuFile->addAction(admin);
    connect(m_share, &ShareBar::settingsRequested, this, &MainWindow::editServerSettings);
    m_net = new QNetworkAccessManager(this);
    ui->mainTabs->setTabEnabled(ui->mainTabs->indexOf(ui->avatarsTab), false);
    ui->mainTabs->setTabEnabled(ui->mainTabs->indexOf(ui->inventoryTab), false);
    syncNav();
    connect(ui->actionInstall, &QAction::triggered, this, &MainWindow::onInstallTriggered);
    connect(ui->actionCredits, &QAction::triggered, this, [this]() { CreditsDialog(this).exec(); });
    connect(ui->actionLaunch, &QAction::triggered, this, &MainWindow::onLaunchTriggered);
    connect(ui->actionSettings, &QAction::triggered, this, [this]() {
        SettingsDialog(this).exec();
        // Внутри диалога игрок мог войти или зарегистрироваться - перечитываем
        // аккаунт, чтобы ник и уровень обновились сразу.
        m_stats->refresh();
    });
    // Профиль и друзья: поиск по нику, запросы, статусы и «зайти к другу».
    auto *friendsAction = new QAction(tr("Profile & Friends..."), this);
    friendsAction->setObjectName("actionFriends");
    ui->menuFile->insertAction(ui->actionSettings, friendsAction);
    connect(friendsAction, &QAction::triggered, this, [this]() {
        auto *dialog = new FriendsDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(dialog, &FriendsDialog::joinRequested, this, [this, dialog](const QString &address) {
            dialog->close();
            joinServer(address);
        });
        connect(dialog, &FriendsDialog::settingsRequested, this, [this, dialog]() {
            dialog->close();
            SettingsDialog(this).exec();
        });
        dialog->show();
        dialog->raise();
        dialog->activateWindow();
    });

    // Встроенный соц-сервер поднимаем всегда - это гарантированный пол:
    // регистрация, вход и чат обязаны работать сразу после первого запуска,
    // без DNS, без VPS и без ручной настройки адреса.
    //
    // Общий сервер - усиление, а не требование: он нужен, чтобы друзья с
    // разных машин нашли друг друга в поиске и списке. Его адрес пробуем
    // отдельно и включаем только если сервер отвечает, поэтому мёртвый домен
    // или упавший VPS просто оставляют локальную базу, а не отключают
    // соц-функции целиком.
    //
    // Свой адрес игрока не трогаем: чужой сервер (не локальный и не наш
    // общий) остаётся в настройках как есть.
    const QString sharedUrl = Settings::sharedSocialUrl();
    const QString stored = Settings::socialUrl();
    const bool unmanaged = stored.isEmpty() || stored.startsWith("http://127.0.0.1")
                           || stored == sharedUrl;

    m_socialServer = new LocalSocialServer(this);
    connect(m_socialServer, &LocalSocialServer::failed, this, [](const QString &reason) {
        qWarning("kogamaz social: %s", qPrintable(reason));
    });
    const QString localUrl = m_socialServer->start();

    if (unmanaged) {
        // Локальным заполняем адрес сразу, чтобы соц-вызовам было куда идти,
        // пока идёт проверка общего сервера: игрок не должен ждать сети.
        if (!localUrl.isEmpty() && (stored.isEmpty()
                                    || stored.startsWith("http://127.0.0.1"))) {
            Settings::setSocialUrl(localUrl);
        }
        if (!sharedUrl.isEmpty()) probeSharedSocial(sharedUrl, localUrl);
    }
    m_stats->refresh();

    // Присутствие: раз в полминуты говорим серверу, что мы онлайн и во что играем.
    // Друзья видят это в своём списке; если лаунчер убили — статус гаснет сам
    // (сервер считает игрока офлайн без пульса дольше 90 секунд).
    m_presenceTimer = new QTimer(this);
    m_presenceTimer->setInterval(30000);
    connect(m_presenceTimer, &QTimer::timeout, this, &MainWindow::announcePresence);
    m_presenceTimer->start();
    announcePresence();
    connect(ui->addVersionButton, &QToolButton::clicked, this, &MainWindow::onInstallTriggered);
    auto *versionMenu = new QMenu(this);
    versionMenu->addAction(ui->actionRenameVersion);
    versionMenu->addAction(ui->actionDuplicateVersion);
    versionMenu->addAction(ui->actionShowVersionFolder);
    versionMenu->addSeparator();
    versionMenu->addAction(ui->actionDeleteVersion);
    ui->moreVersionButton->setMenu(versionMenu);
    connect(ui->firstVersionButton, &QPushButton::clicked, this, &MainWindow::onInstallTriggered);
    connect(ui->noVersionsButton, &QPushButton::clicked, this, &MainWindow::onInstallTriggered);
    connect(ui->openWorldsButton, &QPushButton::clicked, this, &MainWindow::onLaunchTriggered);
    connect(ui->useVersionButton, &QPushButton::clicked, this, &MainWindow::onLaunchTriggered);
    connect(ui->actionRenameVersion, &QAction::triggered, this, &MainWindow::renameInstance);
    connect(ui->actionDuplicateVersion, &QAction::triggered, this, &MainWindow::duplicateInstance);
    connect(ui->actionShowVersionFolder, &QAction::triggered, this, &MainWindow::showInstanceFolder);
    connect(ui->actionDeleteVersion, &QAction::triggered, this, &MainWindow::deleteInstance);

    instances = new QStandardItemModel(this);
    ui->instancesView->setModel(instances);
    ui->instancesView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->instancesView->setIconSize(QSize(32, 32));
    ui->instancesView->setSpacing(0);
    ui->instancesView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->instancesView, &QListView::customContextMenuRequested, this, &MainWindow::showInstanceMenu);
    connect(ui->instancesView->selectionModel(), &QItemSelectionModel::currentChanged, this, [this]() { updateInstanceActions(); });
    connect(instances, &QStandardItemModel::itemChanged, this, &MainWindow::onInstanceRenamed);
    auto *renameShortcut = new QShortcut(QKeySequence(Qt::Key_F2), ui->instancesView);
    renameShortcut->setContext(Qt::WidgetShortcut);
    connect(renameShortcut, &QShortcut::activated, this, &MainWindow::renameInstance);
    auto *deleteShortcut = new QShortcut(QKeySequence::Delete, ui->instancesView);
    deleteShortcut->setContext(Qt::WidgetShortcut);
    connect(deleteShortcut, &QShortcut::activated, this, &MainWindow::deleteInstance);
    connect(ui->instancesView, &QListView::activated, this, [this](const QModelIndex &index) {
        if (launchInstance(index.data(Qt::UserRole).toString()))
            ui->mainTabs->setCurrentWidget(ui->playTab);
    });
    connect(ui->versionSearchLineEdit, &QLineEdit::textChanged, this, [this]() { filterInstances(); });
    connect(ui->versionCombo, &QComboBox::currentIndexChanged, this, [this]() { onVersionChanged(); });
    connect(ui->mainTabs, &QTabWidget::currentChanged, this, [this]() {
        if (m_worlds && ui->mainTabs->currentWidget() == ui->avatarsTab)
            m_worlds->showAvatars();
        if (m_worlds && ui->mainTabs->currentWidget() == ui->inventoryTab)
            m_worlds->showInventory();
        updateInstanceActions();
    });

    QFile catalog(":/versions.json");
    if (catalog.open(QIODevice::ReadOnly))
        m_versionsJson = catalog.readAll();
    loadInstances();
    connect(qApp->styleHints(), &QStyleHints::colorSchemeChanged, this, [this]() { loadInstances(); });
    fetchVersions();
    if (ui->versionCombo->currentIndex() >= 0)
        QTimer::singleShot(0, this, &MainWindow::onLaunchTriggered);
}

void MainWindow::fetchVersions(std::function<void()> done) {
    auto *nam = new QNetworkAccessManager(this);
    QNetworkReply *reply = nam->get(QNetworkRequest(QUrl("https://cdn.openkogama.org/versions.json")));
    auto sslErrors = std::make_shared<QStringList>();
    connect(reply, &QNetworkReply::sslErrors, this, [sslErrors](const QList<QSslError> &errors) {
        for (const QSslError &error : errors)
            sslErrors->append(error.errorString());
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, nam, sslErrors, done]() {
        reply->deleteLater();
        nam->deleteLater();
        if (reply->error() == QNetworkReply::NoError) {
            m_versionsJson = reply->readAll();
            m_versionsError.clear();
        } else {
            m_versionsError = reply->errorString();
            if (!sslErrors->isEmpty())
                m_versionsError += "" + sslErrors->join("");
        }
        if (done) done();
    });
}

void MainWindow::loadInstances(const QString &selectPath) {
    QString wanted = selectPath;
    if (wanted.isEmpty()) wanted = ui->versionCombo->currentData().toString();
    if (wanted.isEmpty()) wanted = Settings::lastInstance();
    QSignalBlocker comboBlocker(ui->versionCombo);
    instances->clear();
    ui->versionCombo->clear();
    QDir dir(Paths::instances());
    QFileIconProvider iconProvider;
    QList<QPair<QString, QJsonObject>> found;
    for (const QString &folder : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile file(dir.filePath(folder) + "/instance.json");
        if (file.open(QIODevice::ReadOnly))
            found.append({folder, QJsonDocument::fromJson(file.readAll()).object()});
    }
    std::stable_sort(found.begin(), found.end(), [](const auto &a, const auto &b) {
        return QVersionNumber::fromString(a.second.value("version").toString()) < QVersionNumber::fromString(b.second.value("version").toString());
    });
    for (const auto &[folder, meta] : std::as_const(found)) {
        QDir instanceDir(dir.filePath(folder));
        QString version = meta.value("version").toString();
        QString name = meta.value("name").toString();
        if (name.isEmpty()) name = version.isEmpty() ? folder : version;

        const QIcon icon = instanceIcon(instanceDir, iconProvider);
        auto *item = new QStandardItem(icon, name);
        item->setToolTip("KoGaMa " + version);
        item->setData(instanceDir.path(), Qt::UserRole);
        item->setData(name, NameRole);
        item->setData(version, VersionRole);
        item->setData(meta.value("unityVersion").toString(), UnityRole);
        instances->appendRow(item);
        ui->versionCombo->addItem(icon, versionTitle(name, version), instanceDir.path());
    }
    int selected = ui->versionCombo->findData(wanted);
    if (selected < 0 && ui->versionCombo->count() > 0) selected = 0;
    ui->versionCombo->setCurrentIndex(selected);
    if (selected >= 0) {
        ui->instancesView->setCurrentIndex(instances->index(selected, 0));
        Settings::setLastInstance(ui->versionCombo->currentData().toString());
    }
    ui->versionCombo->setEnabled(selected >= 0);
    ui->noVersionsPage->setVisible(selected < 0);
    ui->versionSearchLineEdit->setVisible(selected >= 0);
    ui->instancesView->setVisible(selected >= 0);
    ui->selectedVersionGroup->setVisible(selected >= 0);
    filterInstances();
    updatePlayPage();
}

void MainWindow::filterInstances() {
    const QString search = ui->versionSearchLineEdit->text().trimmed();
    int first = -1;
    int visible = 0;
    for (int row = 0; row < instances->rowCount(); ++row) {
        const QModelIndex index = instances->index(row, 0);
        bool matches = index.data(NameRole).toString().contains(search, Qt::CaseInsensitive)
            || index.data(VersionRole).toString().contains(search, Qt::CaseInsensitive);
        ui->instancesView->setRowHidden(row, !matches);
        if (matches) {
            ++visible;
            if (first < 0) first = row;
        }
    }
    const QModelIndex current = ui->instancesView->currentIndex();
    if (!current.isValid() || ui->instancesView->isRowHidden(current.row()))
        ui->instancesView->setCurrentIndex(first >= 0 ? instances->index(first, 0) : QModelIndex());
    int total = instances->rowCount();
    // Счётчик в плейсхолдере: раньше строка собиралась склейкой
    // ("Search " + n + " versions..."), из-за чего lupdate не видел её и
    // плейсхолдер оставался английским при любом языке. Теперь через tr()
    // с плейсхолдером %1.
    ui->versionSearchLineEdit->setPlaceholderText(
        total > 0
            ? (total == 1 ? tr("Search %1 version...").arg(total)
                          : tr("Search %1 versions...").arg(total))
            : tr("Search your versions..."));
    ui->versionStatusLabel->setText(total > 0 && visible == 0
        ? tr("No versions match your search.") : QString());
    ui->versionStatusLabel->setVisible(!ui->versionStatusLabel->text().isEmpty());
    updateInstanceActions();
}

void MainWindow::updateInstanceActions() {
    const QModelIndex index = ui->instancesView->currentIndex();
    bool selected = index.isValid();
    ui->useVersionButton->setEnabled(selected);
    ui->moreVersionButton->setEnabled(selected);
    ui->actionRenameVersion->setEnabled(selected);
    ui->actionDuplicateVersion->setEnabled(selected);
    ui->actionShowVersionFolder->setEnabled(selected);
    bool active = selected && ((m_worlds && index.data(Qt::UserRole).toString() == m_activePath)
        || std::any_of(m_remoteClients.cbegin(), m_remoteClients.cend(), [&](const QPointer<QProcess> &client) {
            return client && client->state() != QProcess::NotRunning
                && client->property("instancePath").toString() == index.data(Qt::UserRole).toString();
        }));
    ui->actionDeleteVersion->setEnabled(selected && !active);
    ui->actionDeleteVersion->setText(active ? "Delete... (switch to another version first)" : "Delete...");
    const QString version = index.data(VersionRole).toString();
    ui->instanceNameLabel->setText(selected ? index.data(NameRole).toString() : "No version selected");
    const QString unity = index.data(UnityRole).toString();
    ui->instanceVersionLabel->setText(selected && !unity.isEmpty() ? "Unity " + unity : QString());
    ui->actionLaunch->setEnabled(ui->mainTabs->currentWidget() == ui->versionsTab ? selected : ui->versionCombo->currentIndex() >= 0);
}

void MainWindow::updatePlayPage() {
    bool installed = ui->versionCombo->currentIndex() >= 0;
    ui->welcomeTitle->setText(installed ? "Ready to play?" : "Let's get started");
    ui->welcomeDescription->setText(installed ? "Open your worlds to play, build or create something new." : "Add a game version, then choose a world to play or build.");
    ui->openWorldsButton->setVisible(installed);
    ui->firstVersionButton->setVisible(!installed);
    m_servers->setCanJoin(true);
}

void MainWindow::onVersionChanged() {
    const QString path = ui->versionCombo->currentData().toString();
    if (path.isEmpty()) return;
    if (hasRemoteGames() && path != Settings::lastInstance()) {
        QMessageBox::information(this, "Change version", "Close the game before changing its version.");
        QSignalBlocker blocker(ui->versionCombo);
        ui->versionCombo->setCurrentIndex(ui->versionCombo->findData(Settings::lastInstance()));
        return;
    }
    if (m_worlds && path != m_activePath && !launchInstance(path)) {
        QSignalBlocker blocker(ui->versionCombo);
        ui->versionCombo->setCurrentIndex(ui->versionCombo->findData(m_activePath));
        return;
    }
    Settings::setLastInstance(path);
    updatePlayPage();
    updateHostVersion();
}

void MainWindow::syncNav()
{
    for (int i = 0; i < m_navButtons.count(); ++i) {
        QToolButton *button = m_navButtons.value(i);
        if (!button) continue;
        button->setChecked(i == ui->mainTabs->currentIndex());
        button->setEnabled(ui->mainTabs->isTabEnabled(i));
    }
}

MainWindow::~MainWindow()
{
    if (m_worlds) m_worlds->reject();
    delete ui;
}

void MainWindow::onInstallTriggered() {
    if (m_installing) return;
    m_installing = true;
    ui->actionInstall->setEnabled(false);
    ui->addVersionButton->setEnabled(false);
    ui->firstVersionButton->setEnabled(false);
    ui->noVersionsButton->setEnabled(false);
    if (m_versionsJson.isEmpty()) {
        fetchVersions([this]() {
            m_installing = false;
            ui->actionInstall->setEnabled(true);
            ui->addVersionButton->setEnabled(true);
            ui->firstVersionButton->setEnabled(true);
            ui->noVersionsButton->setEnabled(true);
            if (m_versionsJson.isEmpty())
                QMessageBox::warning(this, "Add version", "Could not load the version list. Check your internet connection and try again.\n" + m_versionsError);
            else
                onInstallTriggered();
        });
        return;
    }
    InstallDialog dialog(m_versionsJson, this);
    if (dialog.exec() == QDialog::Accepted) {
        const QList<Version> chosen = dialog.selectedVersions();
        QString installed;
        for (const Version &version : chosen) {
            InstallProgressDialog progress(version, chosen.size() == 1 ? dialog.instanceName() : version.version, this);
            if (progress.exec() != QDialog::Accepted) break;
            installed = progress.installedPath();
        }
        if (!installed.isEmpty()) {
            loadInstances(installed);
            ui->mainTabs->setCurrentWidget(ui->playTab);
            onLaunchTriggered();
        }
    }
    m_installing = false;
    ui->actionInstall->setEnabled(true);
    ui->addVersionButton->setEnabled(true);
    ui->firstVersionButton->setEnabled(true);
    ui->noVersionsButton->setEnabled(true);
}

void MainWindow::onLaunchTriggered() {
    const QString path = ui->mainTabs->currentWidget() == ui->versionsTab
        ? ui->instancesView->currentIndex().data(Qt::UserRole).toString()
        : ui->versionCombo->currentData().toString();
    if (path.isEmpty()) return;
    if (launchInstance(path)) {
        ui->mainTabs->setCurrentWidget(ui->playTab);
    } else if (m_worlds) {
        QSignalBlocker blocker(ui->versionCombo);
        ui->versionCombo->setCurrentIndex(ui->versionCombo->findData(m_activePath));
    }
}

void MainWindow::showInstanceMenu(const QPoint &pos) {
    QModelIndex index = ui->instancesView->indexAt(pos);

    if (!index.isValid()) {
        ui->instancesView->setCurrentIndex({});
        QMenu menu(this);
        menu.addAction(ui->actionInstall);
        menu.exec(ui->instancesView->viewport()->mapToGlobal(pos));
        return;
    }
    ui->instancesView->setCurrentIndex(index);

    QMenu menu(this);
    QAction *launch = menu.addAction("Open worlds");
    menu.addSeparator();
    QAction *rename = menu.addAction("Rename");
    QAction *duplicate = menu.addAction("Duplicate");
    QAction *folder = menu.addAction("Show in Explorer");
    QAction *remove = menu.addAction("Delete");

    QAction *chosen = menu.exec(ui->instancesView->viewport()->mapToGlobal(pos));
    if (chosen == launch)
        launchInstance(index.data(Qt::UserRole).toString());
    else if (chosen == rename)
        renameInstance();
    else if (chosen == duplicate)
        duplicateInstance();
    else if (chosen == folder)
        showInstanceFolder();
    else if (chosen == remove)
        deleteInstance();
}

void MainWindow::renameInstance() {
    QModelIndex index = ui->instancesView->currentIndex();
    if (!index.isValid()) return;
    ui->instancesView->edit(index);
}

void MainWindow::onInstanceRenamed(QStandardItem *item) {
    QString name = item->text().trimmed();
    QString previous = item->data(NameRole).toString();
    if (name == previous) return;
    if (name.isEmpty()) {
        item->setText(previous);
        return;
    }

    if (!saveInstanceName(item->data(Qt::UserRole).toString(), name)) {
        QMessageBox::warning(this, "Rename", "Could not save instance.json");
        item->setText(previous);
        return;
    }
    item->setData(name, NameRole);
    int row = ui->versionCombo->findData(item->data(Qt::UserRole));
    if (row >= 0) {
        QSignalBlocker blocker(ui->versionCombo);
        const QString version = item->data(VersionRole).toString();
        ui->versionCombo->setItemText(row, versionTitle(name, version));
    }
    updateInstanceActions();
}

void MainWindow::showInstanceFolder() {
    QModelIndex index = ui->instancesView->currentIndex();
    if (!index.isValid()) return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(index.data(Qt::UserRole).toString()));
}

void MainWindow::duplicateInstance() {
    QModelIndex index = ui->instancesView->currentIndex();
    if (!index.isValid()) return;

    QString source = index.data(Qt::UserRole).toString();
    QString name = index.data(NameRole).toString() + " (copy)";
    QString folder = QFileInfo(source).fileName() + " (copy)";
    QString dest = Paths::instances() + "/" + folder;
    for (int n = 2; QDir(dest).exists(); n++)
        dest = Paths::instances() + "/" + folder + " (" + QString::number(n) + ")";

    auto *progress = new QProgressDialog("Duplicating " + index.data(NameRole).toString() + "...", QString(), 0, 0, this);
    progress->setWindowTitle("Duplicate version");
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->show();

    auto *watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher, progress, dest, name]() {
        progress->deleteLater();
        watcher->deleteLater();
        if (!watcher->result() || !saveInstanceName(dest, name)) {
            QDir(dest).removeRecursively();
            QMessageBox::warning(this, "Duplicate version", "Could not copy the version files");
            return;
        }
        loadInstances();
        const auto copied = instances->match(instances->index(0, 0), Qt::UserRole, dest, 1, Qt::MatchExactly);
        if (!copied.isEmpty()) ui->instancesView->setCurrentIndex(copied.first());
    });
    watcher->setFuture(QtConcurrent::run([source, dest]() { return copyInstance(source, dest); }));
}

void MainWindow::deleteInstance() {
    QModelIndex index = ui->instancesView->currentIndex();
    if (!index.isValid()) return;

    if ((m_worlds && index.data(Qt::UserRole).toString() == m_activePath)
        || std::any_of(m_remoteClients.cbegin(), m_remoteClients.cend(), [&](const QPointer<QProcess> &client) {
            return client && client->state() != QProcess::NotRunning
                && client->property("instancePath").toString() == index.data(Qt::UserRole).toString();
        })) {
        QMessageBox::information(this, "Delete version", "Switch to another game version before deleting this one's files.");
        return;
    }
    auto answer = QMessageBox::question(this, "Delete version",
                                        "Delete \"" + index.data().toString() + "\" and all its files? This can't be undone.",
                                        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    QDir dir(index.data(Qt::UserRole).toString());
    if (QDir::cleanPath(dir.absolutePath()).startsWith(QDir::cleanPath(Paths::instances()) + "/") && !dir.removeRecursively())
        QMessageBox::warning(this, "Delete version", "Some files could not be deleted. Close the game if it is running and try again.");

    loadInstances();
}

bool MainWindow::prepareInstance(const QString &path, QString *executable) {
    QDir dir(path);
    QString exe = dir.entryList({"*.unityweb", "*.unity3d"}, QDir::Files).value(0);

    if (!exe.isEmpty()) {
        if (!QFile::exists(Paths::player())) {
            QMessageBox::warning(this, "Launch", "KoGaMaZ Player is missing from " + QDir::toNativeSeparators(Paths::player()));
            return false;
        }
        if (!WebPlayerRuntime::ensureInstalled(this))
            return false;
    } else {
        QString data = dir.entryList({"*_Data"}, QDir::Dirs).value(0);
        if (data.isEmpty()) {
            QMessageBox::warning(this, "Launch", "This version can't be launched yet");
            return false;
        }

        exe = data.chopped(5) + ".exe";
        if (!dir.exists(exe)) {
            QString other = dir.entryList({"*.exe"}, QDir::Files).value(0);
            if (other.isEmpty() || !dir.rename(other, exe)) {
                QMessageBox::warning(this, "Launch", "No game executable in " + path);
                return false;
            }
        }
    }

    *executable = exe;
    return true;
}

bool MainWindow::launchInstance(const QString &path) {
    if (m_worlds && path == m_activePath) return true;
    if ((m_worlds && m_worlds->hasRunningGames()) || hasRemoteGames()) {
        QMessageBox::information(this, "Change version", "Close the game before changing its version.");
        return false;
    }
    QString exe;
    if (!prepareInstance(path, &exe)) return false;
    const QString version = AssetInstaller::versionOf(path);
    QProcess *server = nullptr;
    if ((!m_localReady || !m_checkedVersions.contains(version)) && !startLocalServer(version, &server))
        return false;

    if (!m_worlds) {
        m_worlds = new WorldsDialog(path, exe, server, this);
        connect(m_worlds, &WorldsDialog::hostRequested, this, [this](int world, const QString &name, QProcess *client) {
            m_hostClient = client;
            m_share->share(world, name, clientVersion(instances->item(ui->versionCombo->currentIndex())));
        });
        connect(m_share, &ShareBar::hostedWorldChanged, m_worlds, &WorldsDialog::setHostedWorld);
        // хостим — друзьям сообщаем, во что играем и как к нам подключиться
        connect(m_share, &ShareBar::hostedWorldChanged, this, [this]() {
            const QString code = m_share->hostedCode();
            m_playingWorld = code.isEmpty() ? QString() : m_share->hostedTitle();
            m_playingAddress = code.isEmpty() ? QString() : Tunnel::address(code);
            announcePresence();
        });
        // Public <-> By code во время хостинга: мир надо сразу перепубликовать,
        // иначе друзья увидят устаревший режим.
        connect(m_share, &ShareBar::visibilityChanged, this, [this](bool) { announcePresence(); });
        m_worlds->setHostedWorld(m_share->hostedWorld());
        m_worlds->setWindowFlags(Qt::Widget);
        m_worlds->hide();
        ui->worldsLayout->addWidget(m_worlds->worldsPage());
        ui->avatarsLayout->addWidget(m_worlds->avatarsPage());
        ui->inventoryLayout->addWidget(m_worlds->inventoryPage());
        m_worlds->worldsPage()->show();
        m_worlds->avatarsPage()->show();
        m_worlds->inventoryPage()->show();
        ui->mainTabs->setTabEnabled(ui->mainTabs->indexOf(ui->avatarsTab), true);
        ui->mainTabs->setTabEnabled(ui->mainTabs->indexOf(ui->inventoryTab), true);
        syncNav();
    } else {
        m_worlds->setInstance(path, exe, server);
    }
    m_activePath = path;
    Settings::setLastInstance(path);
    {
        QSignalBlocker blocker(ui->versionCombo);
        ui->versionCombo->setCurrentIndex(ui->versionCombo->findData(path));
    }
    ui->playStack->setCurrentWidget(ui->worldsPage);
    updateInstanceActions();
    updatePlayPage();
    updateHostVersion();
    return true;
}

bool MainWindow::startLocalServer(const QString &version, QProcess **server) {
    LaunchDialog launch(version, this);
    if (launch.exec() != QDialog::Accepted) return false;
    *server = launch.server();
    m_checkedVersions.insert(version);
    m_localReady = true;
    return true;
}

bool MainWindow::hasRemoteGames() const {
    return std::any_of(m_remoteClients.cbegin(), m_remoteClients.cend(), [](const QPointer<QProcess> &client) {
        return client && client->state() != QProcess::NotRunning;
    });
}

void MainWindow::joinServer(const QString &address) {
    if (Settings::nickname().isEmpty()) {
        bool ok = false;
        const QString name = QInputDialog::getText(this, "Join", "Pick a nickname that other players will see:", QLineEdit::Normal, QString(), &ok).trimmed();
        if (!ok || name.isEmpty()) return;
        Settings::setNickname(name.left(20));
    }
    if (Tunnel::isAddress(address)) {
        joinCode(address);
        return;
    }
    const GameServer server{QString(), address, QString(), {}, m_servers->clientsOf(address)};
    const int current = ui->versionCombo->currentIndex();
    if (current < 0 || !server.accepts(clientVersion(instances->item(current)))) {
        int match = -1;
        for (int row = instances->rowCount() - 1; row >= 0 && match < 0; --row)
            if (server.accepts(clientVersion(instances->item(row)))) match = row;
        if (match < 0) {
            offerVersion(server.clients, m_servers->titleOf(address), [this, address]() { joinServer(address); });
            return;
        }
        ui->versionCombo->setCurrentIndex(match);
    }
    const QString path = ui->versionCombo->currentData().toString();
    if (path.isEmpty()) return;
    QString exe;
    if (!prepareInstance(path, &exe)) return;
    QProcess *client = GameLauncher::start(path, exe, address, "play", 0, this, this);
    if (!client) return;
    if (m_servers->isPublic(address)) {
        DiscordPresence::join(client, address, m_servers->titleOf(address));
        m_servers->reportPlaying(client, address);
    }
    client->setProperty("instancePath", path);
    m_remoteClients.append(client);
    connect(client, &QProcess::finished, this, [this, client]() {
        m_remoteClients.removeAll(client);
        updateInstanceActions();
    });
    Settings::setLastInstance(path);
    updateInstanceActions();
}

void MainWindow::offerVersion(const QStringList &clients, const QString &title, std::function<void()> then) {
    if (m_installing) return;
    const GameServer wanted{QString(), QString(), QString(), {}, clients};
    std::optional<Version> best;
    for (const Version &version : InstallDialog::parse(m_versionsJson))
        if (wanted.accepts(version.version)
            && (!best || QVersionNumber::fromString(version.version) > QVersionNumber::fromString(best->version)))
            best = version;
    const QString server = title.isEmpty() ? QString("This server") : title;
    if (!best) {
        QMessageBox::information(this, "Join", server + " plays KoGaMa " + clients.join(", ") + ". Add it on the Versions tab first.");
        return;
    }
    if (QMessageBox::question(this, "Join", server + " plays KoGaMa " + best->version + ", which you don't have yet. Download it now ("
            + best->download + ")?") != QMessageBox::Yes)
        return;
    m_installing = true;
    InstallProgressDialog progress(*best, best->version, this);
    const bool installed = progress.exec() == QDialog::Accepted;
    m_installing = false;
    if (!installed) return;
    loadInstances(progress.installedPath());
    then();
}

void MainWindow::setUpdateCheck(std::function<void()> check) {
    m_updateCheck = check;
}

void MainWindow::openLink(const QString &link) {
    if (isMinimized()) showNormal();
    raise();
    activateWindow();
    const QString address = Links::serverOf(link);
    if (address.isEmpty()) return;
    ui->mainTabs->setCurrentWidget(m_servers);
    const QString title = m_servers->titleOf(address);
    const QString shown = !title.isEmpty() ? title
        : Tunnel::isAddress(address) ? Tunnel::display(Tunnel::code(address)) : QUrl(address).host();
    if (QMessageBox::question(this, "Join", "Join " + shown + "?") != QMessageBox::Yes) return;
    joinServer(address);
}

void MainWindow::editServerSettings() {
    ServerConfigDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted || !m_localReady || !m_worlds) return;
    if ((m_worlds->hasRunningGames() || hasRemoteGames())
        && QMessageBox::question(this, "Server settings", "Restart the server now? Games on it get disconnected. "
            "Otherwise the settings apply the next time it starts.") != QMessageBox::Yes)
        return;
    m_net->get(QNetworkRequest(QUrl(LocalServerUrl + "/shutdown")));
    QTimer::singleShot(300, this, [this]() { waitForServerToStop(50); });
}

void MainWindow::waitForServerToStop(int attempts) {
    QNetworkRequest ping(QUrl(LocalServerUrl + "/ping"));
    ping.setTransferTimeout(300);
    QNetworkReply *reply = m_net->get(ping);
    connect(reply, &QNetworkReply::finished, this, [this, reply, attempts]() {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError && attempts > 0) {
            QTimer::singleShot(200, this, [this, attempts]() { waitForServerToStop(attempts - 1); });
            return;
        }
        QProcess *server = nullptr;
        QString exe;
        if (!m_worlds || !startLocalServer(AssetInstaller::versionOf(m_activePath), &server) || !prepareInstance(m_activePath, &exe))
            return;
        m_worlds->setInstance(m_activePath, exe, server);
        m_share->restart();
    });
}

void MainWindow::updateHostVersion() {
    m_share->setVersion(clientVersion(instances->item(ui->versionCombo->currentIndex())));
}

void MainWindow::closeEvent(QCloseEvent *event) {
    const int players = m_share->players();
    if (players > 0 && QMessageBox::question(this, "Close launcher",
            QString(players == 1 ? "A player is" : "Players are") + " in your world. Closing the launcher disconnects them. Close anyway?")
            != QMessageBox::Yes) {
        event->ignore();
        return;
    }
    if (m_socialServer) m_socialServer->stop();
    QMainWindow::closeEvent(event);
}

void MainWindow::announcePresence()
{
    if (!Social::enabled() || Settings::socialToken().isEmpty()) return;
    // Пока хостим — код берём напрямую из ShareBar, чтобы не дожидаться
    // переподключения: в момент публикации hostedCode() уже заполнен.
    QString world = m_playingWorld;
    QString address = m_playingAddress;
    if (address.isEmpty() && !m_share->hostedCode().isEmpty()) {
        world = m_share->hostedTitle();
        address = Tunnel::address(m_share->hostedCode());
    }
    // Флаг публичности решает, попадёт ли мир в общий список: публичный виден
    // всем и заходят без кода, приватный — только друзьям через их список.
    const bool isPublic = address.isEmpty() ? false : Settings::hostPublic();
    // при ошибке молчим: наличие соц-сервера не должно ломать саму игру
    Social::presence(world, address, isPublic, [](bool, const QJsonObject &, const QString &) {});
}

void MainWindow::probeSharedSocial(const QString &sharedUrl, const QString &localUrl)
{
    // Таймаут нарочно короткий: мёртвый домен обязан отвалиться быстро, а не
    // держать соц-функции в неопределённом состоянии. Проверяем именно
    // /api/health: это тот же пинг, которым пользуются диалоги.
    auto *net = new QNetworkAccessManager(this);
    QNetworkRequest request{QUrl(sharedUrl + "/api/health")};
    request.setTransferTimeout(3000);
    QNetworkReply *reply = net->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, net, sharedUrl, localUrl]() {
        reply->deleteLater();
        net->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 200) {
            // Общий сервер жив - переключаемся: там общие друзья, посты и
            // список хостящих, ради них он и затевался.
            if (Settings::socialUrl() != sharedUrl) {
                Settings::setSocialUrl(sharedUrl);
                m_stats->refresh();
            }
            return;
        }
        // Общий мёртв. Если прошлый запуск оставил его адрес в настройках,
        // возвращаемся на локальный - иначе соц-функции просто не работали бы.
        qWarning("kogamaz social: shared server %s is unreachable (%s), using the local one",
                 qPrintable(sharedUrl),
                 qPrintable(reply->error() != QNetworkReply::NoError
                                ? reply->errorString() : QStringLiteral("HTTP %1").arg(status)));
        if (Settings::socialUrl() == sharedUrl && !localUrl.isEmpty()) {
            Settings::setSocialUrl(localUrl);
            m_stats->refresh();
        }
    });
}

void MainWindow::joinCode(const QString &address) {
    struct Trip {
        QString path;
        QString exe;
        QString error;
        QString title;
        QStringList needs;
        int world = 0;
        int port = 0;
        bool cancelled = false;
        bool launched = false;
        bool missing = false;
    };
    const QString code = Tunnel::code(address);
    auto trip = std::make_shared<Trip>();
    QPointer<QProgressDialog> progress = new QProgressDialog("Looking for " + Tunnel::display(code) + "...", "Cancel", 0, 0, this);
    progress->setWindowTitle("Join");
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->setAttribute(Qt::WA_DeleteOnClose);
    progress->show();
    QPointer<Tunnel> tunnel = Tunnel::join(code, this);

    auto dismiss = [this, progress]() {
        if (!progress) return;
        progress->disconnect(this);
        progress->close();
    };
    auto launch = [this, address, trip, tunnel, dismiss]() {
        if (trip->launched || trip->path.isEmpty() || trip->port == 0) return;
        dismiss();
        QProcess *client = GameLauncher::start(trip->path, trip->exe, "http://127.0.0.1:" + QString::number(trip->port), "play", trip->world, this, this, true);
        if (!client) {
            if (tunnel) tunnel->stop();
            return;
        }
        trip->launched = true;
        // Друзьям показываем, во что мы зашли и как к нам присоединиться.
        m_playingWorld = m_servers->titleOf(address);
        if (m_playingWorld.isEmpty()) m_playingWorld = Tunnel::display(Tunnel::code(address));
        m_playingAddress = Tunnel::address(Tunnel::code(address));
        announcePresence();
        if (m_servers->isPublic(address)) {
            DiscordPresence::join(client, address, m_servers->titleOf(address));
            m_servers->reportPlaying(client, address);
        }
        m_servers->probeThrough(address, "http://127.0.0.1:" + QString::number(trip->port), trip->world);
        client->setProperty("instancePath", trip->path);
        m_remoteClients.append(client);
        connect(client, &QProcess::finished, this, [this, client, tunnel]() {
            m_remoteClients.removeAll(client);
            if (tunnel) tunnel->stop();
            // игра закрылась — перестаём числиться играющим
            if (m_remoteClients.isEmpty()) {
                m_playingWorld.clear();
                m_playingAddress.clear();
                announcePresence();
            }
            updateInstanceActions();
        });
        Settings::setLastInstance(trip->path);
        updateInstanceActions();
    };

    connect(progress, &QProgressDialog::canceled, this, [trip, tunnel]() {
        trip->cancelled = true;
        if (tunnel) tunnel->stop();
    });
    connect(tunnel, &Tunnel::said, this, [this, address, code, trip, tunnel, progress, launch](const QString &word, const QString &rest) {
        if (word == "ready") {
            trip->port = rest.section(' ', 0, 0).toInt();
            launch();
        } else if (word == "missing") {
            trip->error = "Nobody is hosting with the code " + Tunnel::display(code) + " right now.";
        } else if (word == "note") {
            // туннель сам откатился на встроенные P2P-серверы — покажем это в прогрессе
            if (progress) progress->setLabelText(rest);
        } else if (word == "error") {
            trip->error = rest.startsWith("rendezvous") ? "Could not reach the KoGaMaZ P2P server. Check your internet connection."
                : rest.startsWith("host") ? "Could not connect to the host. Try again in a moment."
                : "Could not join: " + rest;
        } else if (word == "found") {
            const QJsonObject info = QJsonDocument::fromJson(rest.toUtf8()).object();
            m_servers->showFound(address, info);
            const GameServer host{QString(), address, QString(), {}, Tunnel::clients(info)};
            int row = ui->versionCombo->currentIndex();
            if (row < 0 || !host.accepts(clientVersion(instances->item(row)))) {
                row = -1;
                for (int candidate = instances->rowCount() - 1; candidate >= 0 && row < 0; --candidate)
                    if (host.accepts(clientVersion(instances->item(candidate)))) row = candidate;
            }
            if (row < 0) {
                trip->missing = true;
                trip->needs = host.clients;
                trip->title = info.value("title").toString().trimmed();
                if (tunnel) tunnel->stop();
                return;
            }
            const QString path = instances->item(row)->data(Qt::UserRole).toString();
            QString exe;
            if (!launchInstance(path) || !prepareInstance(path, &exe)) {
                trip->cancelled = true;
                if (tunnel) tunnel->stop();
                return;
            }
            trip->path = path;
            trip->exe = exe;
            trip->world = info.value("world").toInt();
            if (progress) progress->setLabelText("Connecting to " + info.value("title").toString() + "...");
            launch();
        }
    });
    connect(tunnel, &Tunnel::ended, this, [this, address, trip, tunnel, dismiss]() {
        dismiss();
        if (tunnel) tunnel->deleteLater();
        if (trip->missing)
            offerVersion(trip->needs, trip->title, [this, address]() { joinCode(address); });
        else if (!trip->launched && !trip->cancelled)
            QMessageBox::information(this, "Join", trip->error.isEmpty() ? "Could not connect to the host." : trip->error);
    });
}
