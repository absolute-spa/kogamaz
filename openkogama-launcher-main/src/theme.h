#ifndef THEME_H
#define THEME_H

class QApplication;
class QStyle;

namespace Theme {

void apply(QApplication &app);
void reapply();   // перечитать настройки (акцент) и обновить стиль на лету
bool dark();
QStyle *style();

}

#endif // THEME_H
