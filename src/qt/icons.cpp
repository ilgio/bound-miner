#include "qt/icons.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>

namespace bound::gui {

namespace {

void arrow(QPainter& p, QPointF from, QPointF to, QPointF head_a, QPointF head_b) {
    p.drawLine(from, to);
    p.drawPolyline(QPolygonF({head_a, to, head_b}));
}

// A cube seen from a corner: a block of the chain.
void block(QPainter& p) {
    p.drawPolygon(QPolygonF({{12, 3.5}, {19.5, 7.75}, {19.5, 16.25}, {12, 20.5}, {4.5, 16.25}, {4.5, 7.75}}));
    p.drawPolyline(QPolygonF({{4.5, 7.75}, {12, 12}, {19.5, 7.75}}));
    p.drawLine(QPointF(12, 12), QPointF(12, 20.5));
}

void draw(QPainter& p, Glyph glyph) {
    switch (glyph) {
    case Glyph::Overview:
        p.drawRoundedRect(QRectF(4, 4, 7, 7), 1.5, 1.5);
        p.drawRoundedRect(QRectF(13, 4, 7, 7), 1.5, 1.5);
        p.drawRoundedRect(QRectF(4, 13, 7, 7), 1.5, 1.5);
        p.drawRoundedRect(QRectF(13, 13, 7, 7), 1.5, 1.5);
        break;
    case Glyph::Send:
    case Glyph::Sent:
        arrow(p, {6, 18}, {18, 6}, {9, 6}, {18, 15});
        break;
    case Glyph::Receive:
    case Glyph::Received:
        arrow(p, {18, 6}, {6, 18}, {6, 9}, {15, 18});
        break;
    case Glyph::Contacts: {
        p.drawEllipse(QPointF(12, 8.5), 3.75, 3.75);
        QPainterPath shoulders;
        shoulders.moveTo(4.5, 20);
        shoulders.cubicTo(5, 14.5, 19, 14.5, 19.5, 20);
        p.drawPath(shoulders);
        break;
    }
    case Glyph::Transactions:
        for (const double y : {6.5, 12.0, 17.5}) {
            p.drawLine(QPointF(9, y), QPointF(20, y));
            p.drawPoint(QPointF(4.75, y));
        }
        break;
    case Glyph::Mining:
    case Glyph::Mined:
        block(p);
        break;
    case Glyph::Network:
        p.drawLine(QPointF(12, 6), QPointF(6, 17));
        p.drawLine(QPointF(12, 6), QPointF(18, 17));
        p.drawLine(QPointF(6, 17), QPointF(18, 17));
        for (const QPointF c : {QPointF(12, 5.5), QPointF(5.5, 17.5), QPointF(18.5, 17.5)}) {
            p.save();
            p.setBrush(p.pen().color());
            p.drawEllipse(c, 2.6, 2.6);
            p.restore();
        }
        break;
    case Glyph::Console:
        p.drawRoundedRect(QRectF(3, 4.5, 18, 15), 2.5, 2.5);
        p.drawPolyline(QPolygonF({{7, 9.5}, {10, 12}, {7, 14.5}}));
        p.drawLine(QPointF(12, 15), QPointF(16.5, 15));
        break;
    }
}

}  // namespace

QPixmap glyph_pixmap(Glyph glyph, const QColor& color, int size) {
    const qreal dpr = qApp != nullptr ? qApp->devicePixelRatio() : 1.0;
    QPixmap pixmap(QSize(size, size) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(size / 24.0, size / 24.0);
    QPen pen(color, 1.8);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    draw(p, glyph);
    return pixmap;
}

QIcon glyph_icon(Glyph glyph, const QColor& color, const QColor& selected, int size) {
    QIcon icon;
    icon.addPixmap(glyph_pixmap(glyph, color, size), QIcon::Normal);
    icon.addPixmap(glyph_pixmap(glyph, color, size), QIcon::Active);
    icon.addPixmap(glyph_pixmap(glyph, selected, size), QIcon::Selected);
    return icon;
}

}  // namespace bound::gui
