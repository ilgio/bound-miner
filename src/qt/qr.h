#pragma once

// Milestone 18: QR codes for the Receive page, made by Project Nayuki's QR Code generator
// (qt/qrcodegen, MIT license).

#include <QImage>
#include <QString>

#include <vector>

namespace bound::gui {

// The code's modules, row by row (true: dark), at error correction level M (15% may be lost).
// Throws std::length_error if the text doesn't fit in a QR code.
std::vector<std::vector<bool>> qr_modules(const QString& text);

// The code as a picture: dark on white whatever the theme (scanners expect that), `scale` pixels a
// module, with the 4-module white border the standard asks for.
QImage qr_image(const QString& text, int scale);

}  // namespace bound::gui
