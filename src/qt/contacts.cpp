#include "qt/contacts.h"

#include <QSettings>

#include <algorithm>
#include <utility>

namespace bound::gui {

namespace {

const QString kGroup = QStringLiteral("contacts");

void sort_by_name(std::vector<Contact>& list) {
    std::sort(list.begin(), list.end(), [](const Contact& a, const Contact& b) {
        const int order = QString::compare(a.name, b.name, Qt::CaseInsensitive);
        return order != 0 ? order < 0 : a.address < b.address;
    });
}

}  // namespace

Contacts::Contacts(QString settings_file) : file_(std::move(settings_file)) {
    QSettings settings(file_, QSettings::IniFormat);
    const int count = settings.beginReadArray(kGroup);
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        Contact c{settings.value(QStringLiteral("name")).toString().trimmed(), settings.value(QStringLiteral("address")).toString().trimmed()};
        if (c.name.isEmpty() || c.address.isEmpty() || name_for(c.address)) continue;
        list_.push_back(std::move(c));
    }
    settings.endArray();
    sort_by_name(list_);
}

std::optional<QString> Contacts::name_for(const QString& address) const {
    const auto found = std::find_if(list_.begin(), list_.end(), [&](const Contact& c) { return c.address == address; });
    if (found == list_.end()) return std::nullopt;
    return found->name;
}

void Contacts::put(const Contact& contact) {
    const Contact clean{contact.name.trimmed(), contact.address.trimmed()};
    if (clean.name.isEmpty() || clean.address.isEmpty()) return;
    const auto found = std::find_if(list_.begin(), list_.end(), [&](const Contact& c) { return c.address == clean.address; });
    if (found != list_.end()) {
        found->name = clean.name;
    } else {
        list_.push_back(clean);
    }
    sort_by_name(list_);
    ++version_;
    save();
}

void Contacts::remove(const QString& address) {
    const auto before = list_.size();
    std::erase_if(list_, [&](const Contact& c) { return c.address == address; });
    if (list_.size() == before) return;
    ++version_;
    save();
}

void Contacts::save() const {
    QSettings settings(file_, QSettings::IniFormat);
    settings.remove(kGroup);
    settings.beginWriteArray(kGroup, static_cast<int>(list_.size()));
    for (int i = 0; i < static_cast<int>(list_.size()); ++i) {
        settings.setArrayIndex(i);
        settings.setValue(QStringLiteral("name"), list_[static_cast<std::size_t>(i)].name);
        settings.setValue(QStringLiteral("address"), list_[static_cast<std::size_t>(i)].address);
    }
    settings.endArray();
    settings.sync();
}

}  // namespace bound::gui
