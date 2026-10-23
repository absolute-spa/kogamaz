#ifndef THEME_H
#define THEME_H

#include <QSize>

class QApplication;
class QStyle;
class QWidget;

namespace Theme {

void apply(QApplication &app);
void reapply();   // перечитать настройки (акцент) и обновить стиль на лету
bool dark();
QStyle *style();

// Размер окна с поправкой на экран: preferred, но не больше видимой области.
// На ноутбуке с масштабом 150% жёсткий resize(470, 880) уводил низ окна за
// край экрана, и до кнопок было не добраться.
void fitToScreen(QWidget *window, const QSize &preferred);

}

#endif // THEME_H
