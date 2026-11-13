#pragma once

#include <QString>

namespace vnxfer::keyring {

bool available();
QString lookup(const QString& endpoint);
bool store(const QString& endpoint, const QString& password);
void remove(const QString& endpoint);

}  // namespace vnxfer::keyring
