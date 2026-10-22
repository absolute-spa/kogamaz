#include "creditsdialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPixmap>
#include <QVBoxLayout>

CreditsDialog::CreditsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("Credits");
    setWindowIcon(QIcon(":/logo.png"));
    setFixedWidth(380);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 16);
    layout->setSpacing(6);

    auto *logo = new QLabel(this);
    logo->setPixmap(QPixmap(":/logo.png").scaled(72, 72, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    logo->setAlignment(Qt::AlignCenter);
    layout->addWidget(logo);

    auto *title = new QLabel("KoGaMaZ", this);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 8);
    title->setFont(titleFont);
    title->setAlignment(Qt::AlignCenter);
    layout->addWidget(title);

    auto *author = new QLabel("Created by FeelZ Dev", this);
    author->setForegroundRole(QPalette::PlaceholderText);
    author->setAlignment(Qt::AlignCenter);
    layout->addWidget(author);

    layout->addStretch();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}
