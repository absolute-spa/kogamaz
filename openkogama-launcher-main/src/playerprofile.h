#ifndef PLAYERPROFILE_H
#define PLAYERPROFILE_H

#include <QString>

class QObject;

namespace PlayerProfile {

void send(const QString &url, const QString &key, bool everything);
void keep(const QString &url, const QString &key, bool local, QObject *owner);
void refresh(QObject *owner);

}

#endif // PLAYERPROFILE_H
