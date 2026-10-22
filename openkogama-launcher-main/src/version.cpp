#include "version.h"

#include <QVector>

namespace {

// Разбирает "1.2.3" на числа. Нечисловые куски (метки вроде beta) - стоп,
// дальше только мусор, он лаунчеру не нужен.
QVector<int> parts(const QString &text)
{
    QVector<int> out;
    for (const QString &chunk : text.split(QLatin1Char('.'))) {
        QString digits;
        for (const QChar letter : chunk)
            if (letter.isDigit()) digits += letter;
            else break;
        if (digits.isEmpty()) break;
        out.append(digits.toInt());
    }
    return out;
}

}

namespace Version {

QString normalize(const QString &text)
{
    QString clean = text.trimmed();
    // Тег релиза может быть "v1.2.0" или "V1.2.0" - букву съедаем.
    if (clean.startsWith(QLatin1Char('v')) || clean.startsWith(QLatin1Char('V')))
        clean = clean.mid(1);
    // Метка вида -beta или -rc1 не участвует в сравнении: лаунчеру важен
    // только основной номер.
    const int marker = clean.indexOf(QLatin1Char('-'));
    if (marker >= 0) clean = clean.left(marker);
    return clean.trimmed();
}

int compare(const QString &left, const QString &right)
{
    const QVector<int> a = parts(normalize(left));
    const QVector<int> b = parts(normalize(right));
    const int count = qMax(a.size(), b.size());
    for (int i = 0; i < count; ++i) {
        // Отсутствующая часть = 0, чтобы 1.2 и 1.2.0 считались равными.
        const int lhs = i < a.size() ? a.at(i) : 0;
        const int rhs = i < b.size() ? b.at(i) : 0;
        if (lhs != rhs) return lhs < rhs ? -1 : 1;
    }
    return 0;
}

bool isNewer(const QString &candidate, const QString &current)
{
    const QString have = normalize(current);
    if (have.isEmpty()) return !normalize(candidate).isEmpty();
    return compare(candidate, have) > 0;
}

}
