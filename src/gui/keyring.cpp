#ifdef VNXFER_HAVE_LIBSECRET
// glib headers use "signals" as a struct field name, which collides with the
// Qt keyword macro, so pull them in before any Qt header.
#include <libsecret/secret.h>
#endif

#include "gui/keyring.h"

namespace vnxfer::keyring {

#ifdef VNXFER_HAVE_LIBSECRET
namespace {
SecretSchema* schema() {
    static SecretSchema* s = secret_schema_new("org.vnxfer.Password", SECRET_SCHEMA_NONE,
                                               "endpoint", SECRET_SCHEMA_ATTRIBUTE_STRING, nullptr);
    return s;
}
}  // namespace

bool available() { return true; }

QString lookup(const QString& endpoint) {
    GError* error = nullptr;
    gchar* password = secret_password_lookup_sync(schema(), nullptr, &error, "endpoint",
                                                  endpoint.toUtf8().constData(), nullptr);
    QString result;
    if (error) {
        g_error_free(error);
    } else if (password) {
        result = QString::fromUtf8(password);
        secret_password_free(password);
    }
    return result;
}

bool store(const QString& endpoint, const QString& password) {
    GError* error = nullptr;
    const gboolean ok = secret_password_store_sync(
        schema(), SECRET_COLLECTION_DEFAULT, "vnxfer", password.toUtf8().constData(), nullptr,
        &error, "endpoint", endpoint.toUtf8().constData(), nullptr);
    if (error) g_error_free(error);
    return ok != FALSE;
}

void remove(const QString& endpoint) {
    GError* error = nullptr;
    secret_password_clear_sync(schema(), nullptr, &error, "endpoint",
                               endpoint.toUtf8().constData(), nullptr);
    if (error) g_error_free(error);
}

#else

bool available() { return false; }
QString lookup(const QString&) { return {}; }
bool store(const QString&, const QString&) { return false; }
void remove(const QString&) {}

#endif

}  // namespace vnxfer::keyring
