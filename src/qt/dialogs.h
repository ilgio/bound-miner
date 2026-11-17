#pragma once

// The wallet app's dialogs: passphrases, creating and restoring a wallet, the seed, one
// transaction in detail, and About.

#include "qt/contacts.h"

#include <QString>

#include <optional>

class QWidget;

namespace bound::gui {

class Session;

// The passphrase, or nullopt if the user cancelled. `purpose` says what it unlocks the wallet for.
std::optional<QString> ask_passphrase(QWidget* parent, const QString& purpose);

// First start, no wallet yet: create one, restore one from its seed, or leave it for later.
void offer_wallet(Session& session, QWidget* parent);
void create_wallet(Session& session, QWidget* parent);
void restore_wallet(Session& session, QWidget* parent);

// The seed, shown so it can be written down. A fresh one can't be closed before the user says so.
void show_seed(QWidget* parent, const QString& seed, bool fresh);
void backup_seed(Session& session, QWidget* parent);
void change_passphrase(Session& session, QWidget* parent);

void show_transaction(Session& session, QWidget* parent, const QString& txid);
void show_about(QWidget* parent);

// Milestone 18: a contact's name and address, checked; nullopt if the user cancelled. `contact`
// fills the fields in (an empty name: a new contact).
std::optional<Contact> edit_contact(const Session& session, QWidget* parent, const Contact& contact);

}  // namespace bound::gui
