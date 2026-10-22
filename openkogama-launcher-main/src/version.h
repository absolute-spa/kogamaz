#ifndef VERSION_H
#define VERSION_H

#include <QString>

// Сравнение версий вида 1.2.0. Нужно, чтобы понять, вышло ли обновление.
//
// Теги релизов GitHub пишутся с буквой v (v1.2.0), а версия в exe - без неё,
// поэтому ведущую v отбрасываем молча. Прочий мусор после номера (метки
// вроде -beta) отрезаем: лаунчеру важно только "старше или младше".
namespace Version {

// Нормализует строку к чистому "1.2.3": убирает v/V, пробелы и хвост после '-'.
QString normalize(const QString &text);

// -1 - left старше, 0 - равны, 1 - right старше. Сравнение числовое по
// частям, поэтому 1.10.0 новее 1.9.0 (это не лексикографический порядок).
int compare(const QString &left, const QString &right);

// true, если candidate новее current. Пустая current считается "всё новее":
// без зашитой версии лучше показать обновление, чем молчать.
bool isNewer(const QString &candidate, const QString &current);

}

#endif // VERSION_H
