#pragma once

// Milestone 18: the wallet app's contacts, names for the addresses you pay. They live in the app's
// settings file in the network's data directory (bound-qt.ini), apart from the wallet: losing them
// loses no coins, and the 24 words don't bring them back.

#include <QString>

#include <optional>
#include <vector>

namespace bound::gui {

struct Contact {
    QString name;
    QString address;
    friend bool operator==(const Contact&, const Contact&) = default;
};

class Contacts {
public:
    explicit Contacts(QString settings_file);  // reads what's saved there

    const std::vector<Contact>& list() const { return list_; }  // by name, ignoring case
    std::optional<QString> name_for(const QString& address) const;
    // Adds the contact, or renames the one that has its address. Saved at once.
    void put(const Contact& contact);
    void remove(const QString& address);
    // Goes up on every change, so a page can tell its names are out of date.
    int version() const { return version_; }

private:
    void save() const;

    QString file_;
    std::vector<Contact> list_;
    int version_ = 0;
};

}  // namespace bound::gui
