#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QSize>

class QListWidget;
class QObject;

namespace TileGrid {

inline const QSize WideImage(400, 160);
inline const QSize SquareImage(240, 240);
inline const QSize WideTile(190, 76);
inline const QSize SquareTile(120, 120);

void setup(QListWidget *list, QObject *resizeFilter);
void fit(QListWidget *list, const QSize &tile);
QPixmap backdrop(const QSize &size, const QColor &background);
QPixmap placeholder(const QSize &size, const QColor &background);
QPixmap cover(const QPixmap &image, const QSize &size);
QIcon icon(const QPixmap &pixmap);

}
