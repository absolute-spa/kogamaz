#include "chatdialog.h"
#include "theme.h"

#include "settings.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// Каждое сообщение — свой виджет: собеседник видит свой текст слева,
// своё — справа и подсвечен. Списку нужен отдельный виджет на элемент.
QWidget *makeBubble(const Social::Message &message, QWidget *parent)
{
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(8, 3, 8, 3);

    auto *bubble = new QLabel(row);
    bubble->setObjectName("chatBubble");
    bubble->setWordWrap(true);
    bubble->setTextInteractionFlags(Qt::TextSelectableByMouse);

    const QString time = QDateTime::fromMSecsSinceEpoch(message.created)
                             .toString(QStringLiteral("dd.MM HH:mm"));
    QString text = message.body.toHtmlEscaped().replace('\n', QStringLiteral("<br>"));
    text += QStringLiteral("<br><span style=\"color:#8b949e;font-size:10px;\">") + time;
    if (message.mine) {
        // у своих показываем, прочитал ли собеседник — как в мессенджерах
        text += message.read ? QObject::tr(" · read") : QObject::tr(" · sent");
    }
    text += QLatin1String("</span>");
    bubble->setText(text);

    // своё — к правому краю, чужое — к левому
    if (message.mine) {
        bubble->setStyleSheet(QStringLiteral(
            "background:#1f3a5f; color:#e6edf3; padding:6px 10px; border-radius:8px;"));
        layout->addStretch(1);
        layout->addWidget(bubble, 1);
    } else {
        bubble->setStyleSheet(QStringLiteral(
            "background:#21262d; color:#e6edf3; padding:6px 10px; border-radius:8px;"));
        layout->addWidget(bubble, 1);
        layout->addStretch(1);
    }
    return row;
}

} // namespace

ChatDialog::ChatDialog(const QString &peerId, const QString &peerNick, const QString &peerHandle,
                       QWidget *parent)
    : QDialog(parent), m_peerId(peerId),
      m_peerNick(peerHandle.isEmpty() ? peerNick : peerNick + " (@" + peerHandle + ")")
{
    setWindowTitle(tr("Chat with %1").arg(m_peerNick));
    setObjectName("chatDialog");
    Theme::fitToScreen(this, {420, 560});

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(10, 10, 10, 10);
    outer->setSpacing(8);

    auto *who = new QLabel(m_peerNick, this);
    who->setObjectName("chatPeer");
    who->setStyleSheet(QStringLiteral("font-weight:bold;"));
    outer->addWidget(who);

    m_status = new QLabel(this);
    m_status->setObjectName("chatStatus");
    m_status->setWordWrap(true);
    m_status->setStyleSheet(QStringLiteral("color: #f85149;"));
    outer->addWidget(m_status);

    auto *logScroll = new QScrollArea(this);
    logScroll->setObjectName("chatScroll");
    logScroll->setWidgetResizable(true);
    logScroll->setFrameShape(QFrame::NoFrame);
    m_log = new QListWidget(logScroll);
    m_log->setObjectName("chatLog");
    m_log->setResizeMode(QListView::Adjust);
    m_log->setMovement(QListView::Static);
    m_log->setSpacing(2);
    // у элемента-строки нет рамки и фона — рисует сам пузырь
    m_log->setStyleSheet(QStringLiteral(
        "QListWidget { background: transparent; border: none; }"
        "QListWidget::item { border: none; background: transparent; }"));
    logScroll->setWidget(m_log);
    outer->addWidget(logScroll, 1);

    // кнопка догрузки старой истории появляется над списком, пока есть что листать
    m_loadOlder = new QPushButton(tr("Load older messages"), this);
    m_loadOlder->setObjectName("chatLoadOlderButton");
    m_loadOlder->setVisible(false);
    outer->addWidget(m_loadOlder);
    connect(m_loadOlder, &QPushButton::clicked, this, [this]() { loadHistory(m_oldest); });

    auto *composer = new QHBoxLayout;
    m_input = new QLineEdit(this);
    m_input->setObjectName("chatInput");
    m_input->setPlaceholderText(tr("Type a message..."));
    m_input->setClearButtonEnabled(true);
    composer->addWidget(m_input, 1);
    m_send = new QPushButton(tr("Send"), this);
    m_send->setObjectName("chatSendButton");
    composer->addWidget(m_send);
    outer->addLayout(composer);

    // Enter отправляет, а не жмёт кнопку «OK» диалога и не закрывает окно
    connect(m_send, &QPushButton::clicked, this, &ChatDialog::send);
    connect(m_input, &QLineEdit::returnPressed, this, &ChatDialog::send);

    // автообновление: новая реплика собеседника появляется сама, без кнопки
    m_autoRefresh = new QTimer(this);
    m_autoRefresh->setInterval(5000);
    connect(m_autoRefresh, &QTimer::timeout, this, &ChatDialog::reload);

    reload();
    m_autoRefresh->start();
}

void ChatDialog::setBusy(bool busy)
{
    m_busy = busy;
    m_send->setEnabled(!busy);
    m_input->setEnabled(!busy);
}

// Полная перезагрузка хвоста истории: нужна после отправки, чтобы сервер
// подтвердил прочтение и доставку, и по таймеру. Старые сообщения при этом
// не трогаем — иначе список дёргается вверх при каждом опросе.
void ChatDialog::reload()
{
    if (m_busy || m_loadingOlder) return;
    setBusy(true);

    const QPointer<ChatDialog> self(this);
    // грузим хвост, старое остаётся на месте
    Social::chatHistory(m_peerId, 0, [self](bool ok, const QJsonObject &json, const QString &error) {
        if (!self) return;
        self->setBusy(false);
        if (!ok) {
            self->m_status->setText(error);
            return;
        }
        self->m_status->clear();
        const Social::ChatPage page = Social::chatPageOf(json);
        const bool atBottom = self->m_log->verticalScrollBar()->value()
            >= self->m_log->verticalScrollBar()->maximum() - 4;
        self->appendMessages(page.messages);
        // курсор для догрузки истории — самое старое из того, что уже есть
        for (const Social::Message &message : page.messages) {
            if (self->m_oldest == 0 || message.created < self->m_oldest)
                self->m_oldest = message.created;
        }
        self->m_loadOlder->setVisible(page.hasMore && self->m_oldest > 0);
        // прокручиваем вниз только если и так были внизу — не дёргаем чужую прокрутку
        if (atBottom) self->scrollToBottom();
        // входящие помечаем прочитанными, иначе счётчик нечитанных не сойдёт.
        // Callback обязателен: call() пропускает вызов с пустой функцией.
        bool hasIncoming = false;
        for (const Social::Message &message : page.messages)
            if (!message.mine) { hasIncoming = true; break; }
        if (hasIncoming)
            Social::markRead(self->m_peerId, [](bool, const QJsonObject &, const QString &) {});
    });
}

void ChatDialog::loadHistory(qint64 before)
{
    if (m_loadingOlder || before <= 0) return;
    m_loadingOlder = true;
    m_loadOlder->setEnabled(false);

    const QPointer<ChatDialog> self(this);
    Social::chatHistory(m_peerId, before, [self](bool ok, const QJsonObject &json, const QString &error) {
        if (!self) return;
        self->m_loadingOlder = false;
        self->m_loadOlder->setEnabled(true);
        if (!ok) {
            self->m_status->setText(error);
            return;
        }
        const Social::ChatPage page = Social::chatPageOf(json);
        // вставляем наверх, запоминая, где были: прокрутку держим на месте
        const int previousHeight = self->m_log->verticalScrollBar()->maximum();
        const int previousTop = self->m_log->verticalScrollBar()->value();
        self->prependMessages(page.messages);
        self->m_loadOlder->setVisible(page.hasMore);
        if (!page.messages.isEmpty()) self->m_oldest = page.messages.first().created;
        const int delta = self->m_log->verticalScrollBar()->maximum() - previousHeight;
        self->m_log->verticalScrollBar()->setValue(previousTop + delta);
    });
}

void ChatDialog::send()
{
    const QString text = m_input->text().trimmed();
    if (text.isEmpty() || m_busy) return;
    setBusy(true);
    m_status->clear();
    m_input->clear();

    const QPointer<ChatDialog> self(this);
    Social::sendMessage(m_peerId, text, [self, text](bool ok, const QJsonObject &json, const QString &error) {
        if (!self) return;
        if (!ok) {
            // текст возвращаем: потерять набранное из-за сетевой ошибки обидно
            self->m_input->setText(text);
            self->setBusy(false);
            self->m_status->setText(error);
            return;
        }
        // показываем сразу, сервер подтвердил сохранение
        Social::Message mine;
        mine.id = json.value("id").toString();
        mine.mine = true;
        mine.body = json.value("body").toString();
        mine.created = static_cast<qint64>(json.value("created").toDouble());
        mine.read = false;
        self->appendMessages({mine});
        self->scrollToBottom();
        self->setBusy(false);
        self->m_input->setFocus();
    });
}

// Дозаписывает сообщения в конец списка, пропуская уже известные по id:
// автообновление приносит хвост, который частично уже на экране.
void ChatDialog::appendMessages(const QList<Social::Message> &messages)
{
    for (const Social::Message &message : messages) {
        bool known = false;
        for (int i = 0; i < m_log->count(); ++i) {
            auto *item = m_log->item(i);
            if (item->data(Qt::UserRole).toString() == message.id) { known = true; break; }
        }
        if (known) continue;
        auto *item = new QListWidgetItem(m_log);
        item->setData(Qt::UserRole, message.id);
        QWidget *bubble = makeBubble(message, m_log);
        item->setSizeHint(bubble->sizeHint());
        m_log->setItemWidget(item, bubble);
    }
}

// Вставляет старую историю перед уже загруженной, сохраняя порядок.
// Дубликаты пропускаются: автообновление по таймеру и ручная прокрутка
// могут принести одно и то же сообщение, если ответы пришли вразнобой.
void ChatDialog::prependMessages(const QList<Social::Message> &messages)
{
    int index = 0;
    for (const Social::Message &message : messages) {
        bool known = false;
        for (int i = 0; i < m_log->count(); ++i) {
            if (m_log->item(i)->data(Qt::UserRole).toString() == message.id) { known = true; break; }
        }
        if (known) continue;
        auto *item = new QListWidgetItem();
        item->setData(Qt::UserRole, message.id);
        QWidget *bubble = makeBubble(message, m_log);
        item->setSizeHint(bubble->sizeHint());
        m_log->insertItem(index++, item);
        m_log->setItemWidget(item, bubble);
    }
}

void ChatDialog::scrollToBottom()
{
    m_log->scrollToBottom();
}
