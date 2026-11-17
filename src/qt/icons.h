#pragma once

// Milestone 18: the wallet app's icons, drawn in code as thin line pictures on a 24-unit grid, so
// they're sharp at any size and take the colour they're asked for.

#include <QColor>
#include <QIcon>
#include <QPixmap>

namespace bound::gui {

enum class Glyph { Overview, Send, Receive, Contacts, Transactions, Mining, Network, Console, Received, Sent, Mined };

QPixmap glyph_pixmap(Glyph glyph, const QColor& color, int size);
// `color` normally, `selected` while its list item is selected (the sidebar's).
QIcon glyph_icon(Glyph glyph, const QColor& color, const QColor& selected, int size = 18);

}  // namespace bound::gui
