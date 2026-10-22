#include "brandlogo.h"

#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRandomGenerator>

namespace {

// Цвета тега: чёрная заливка с жёлтым контуром, как на скриншоте логотипа.
const QColor Fill(0x12, 0x12, 0x12);
const QColor Outline(0xff, 0xd4, 0x00);
const QColor Drip(0xff, 0xc4, 0x00);
const QColor Shadow(0x00, 0x00, 0x00, 0x66);

// Impact есть на каждой Windows; без него возьмём любой тяжёлый шрифт.
QFont heavyFont()
{
    const QStringList candidates{"Impact", "Arial Black", "Haettenschweiler"};
    const QFontDatabase &database = QFontDatabase();
    for (const QString &family : candidates) {
        if (database.families().contains(family, Qt::CaseInsensitive)) {
            QFont candidate(family);
            if (QFontMetrics(candidate).ascent() > 0) return candidate;
        }
    }
    QFont fallback;
    fallback.setBold(true);
    return fallback;
}

} // namespace

QPixmap BrandLogo::render(const QSize &size)
{
    if (!size.isValid()) return QPixmap();

    // Канва крупнее цели: на ней рисуем текст в 3x, потом сжимаем - так края
    // букв не превращаются в кашу при уменьшении.
    constexpr int Scale = 3;
    const int width = size.width() * Scale;
    const int height = size.height() * Scale;

    QPixmap canvas(width, height);
    canvas.fill(Qt::transparent);

    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    QFont font = heavyFont();
    font.setBold(true);
    // Подбираем кегль так, чтобы слово заполнило ширину без обрезки.
    const QString text = QStringLiteral("KOGAMAZ");
    for (int point = 40; point >= 6; --point) {
        font.setPointSize(point);
        if (QFontMetrics(font).horizontalAdvance(text) <= width * 0.94) break;
    }
    font.setLetterSpacing(QFont::AbsoluteSpacing, height * 0.03);

    // Капли краски: несколько вертикальных полосок под буквами, у каждой своя
    // длина и закруглённый конец - без них тег выглядит просто жирным шрифтом.
    const QFontMetrics metrics(font);
    const int textWidth = metrics.horizontalAdvance(text);
    const int left = (width - textWidth) / 2;
    const int baseline = height * 0.66;
    const int capHeight = metrics.ascent();

    painter.setFont(font);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Shadow);
    painter.save();
    painter.translate(height * 0.04, height * 0.04);
    painter.drawText(QRect(0, baseline - capHeight, width, height), Qt::AlignLeft, text);
    painter.restore();

    // Слово слегка наклонено - по-нему сразу видно, что это тег, а не заголовок.
    painter.save();
    painter.translate(width / 2.0, height / 2.0);
    painter.rotate(-3.5);
    painter.translate(-width / 2.0, -height / 2.0);

    // Капли рисуем до букв, чтобы они выглядывали из-под штриха.
    const QChar *chars = text.unicode();
    int x = left;
    painter.setBrush(Drip);
    for (int i = 0; i < text.size(); ++i) {
        const int advance = metrics.horizontalAdvance(chars[i]);
        // Ограничим генератор детерминированно по позиции буквы: логотип не
        // должен меняться от перерисовки к перерисовке.
        const quint32 seed = static_cast<quint32>(i * 2654435761u);
        const int tail = static_cast<int>((seed >> 8) % 4);      // 0..3 капли
        if (tail > 0 && (seed & 1u)) {
            const int dropWidth = qMax(2, advance / 4);
            const int dropHeight = height * (0.10 + tail * 0.06);
            painter.drawRoundedRect(QRect(x + advance / 3, baseline - 2, dropWidth, dropHeight),
                                    dropWidth / 2, dropWidth / 2);
        }
        x += advance;
    }

    // Обводка жёлтым и чёрная заливка поверх - двойной штрих как у маркера.
    QPen outline(Outline);
    outline.setWidthF(height * 0.055);
    outline.setJoinStyle(Qt::MiterJoin);
    painter.setPen(outline);
    painter.setBrush(Fill);
    painter.drawText(QRect(0, baseline - capHeight, width, height), Qt::AlignLeft, text);
    painter.restore();

    painter.end();

    return canvas.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}
