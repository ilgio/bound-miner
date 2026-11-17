#include "qt/qr.h"

#include "qt/qrcodegen/qrcodegen.hpp"

#include <QPainter>

namespace bound::gui {

namespace {

constexpr int kQuietZone = 4;

}  // namespace

std::vector<std::vector<bool>> qr_modules(const QString& text) {
    const QByteArray utf8 = text.toUtf8();
    const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(utf8.constData(), qrcodegen::QrCode::Ecc::MEDIUM);
    const int size = qr.getSize();
    std::vector<std::vector<bool>> rows(static_cast<std::size_t>(size), std::vector<bool>(static_cast<std::size_t>(size)));
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) rows[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] = qr.getModule(x, y);
    }
    return rows;
}

QImage qr_image(const QString& text, int scale) {
    const std::vector<std::vector<bool>> rows = qr_modules(text);
    const int size = static_cast<int>(rows.size());
    const int side = (size + 2 * kQuietZone) * scale;
    QImage image(side, side, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter p(&image);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (rows[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)]) {
                p.fillRect((x + kQuietZone) * scale, (y + kQuietZone) * scale, scale, scale, Qt::black);
            }
        }
    }
    return image;
}

}  // namespace bound::gui
