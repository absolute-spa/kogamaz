#pragma once

#include <QString>

class QProcess;

namespace DiscordPresence {
void show(QProcess *client, const QString &details, const QString &state);
void join(QProcess *client, const QString &address, const QString &title);
void refresh();
}
