#ifndef APPVERSION_H
#define APPVERSION_H

// Версия лаунчера. Единственное место, где её надо править перед релизом:
// она зашивается в exe, сравнивается с тегом релиза GitHub (v1.2.0) и
// показывается в диалоге обновления.
//
// Тег релиза пишется с буквой v (v1.0.0), здесь - без неё.
namespace AppVersion {

inline const char *String = "1.0.1";

}

#endif // APPVERSION_H
