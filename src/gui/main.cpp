#include <QApplication>

#include "gui/main_window.h"
#include "gui/session_worker.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName("vnxfer");
    QApplication::setApplicationName("vnxfer-gui");

    qRegisterMetaType<RemoteEntryQt>("RemoteEntryQt");
    qRegisterMetaType<QVector<RemoteEntryQt>>("QVector<RemoteEntryQt>");

    MainWindow window;
    window.show();

    const QStringList args = app.arguments();
    for (int i = 1; i + 1 < args.size(); ++i) {
        if (args[i] == "--path" && i + 1 < args.size()) {
            window.setInitialPath(args[i + 1]);
        } else if (args[i] == "--connect") {
            const QString endpoint = args[i + 1];
            const int pos = endpoint.lastIndexOf(':');
            const QString host = pos > 0 ? endpoint.left(pos) : endpoint;
            const int port = pos > 0 ? endpoint.mid(pos + 1).toInt() : 5900;
            QString password;
            if (i + 3 < args.size() && args[i + 2] == "--password") password = args[i + 3];
            window.autoConnect(host, port > 0 ? port : 5900, password);
        } else if (args[i] == "--selftest" && i + 2 < args.size()) {
            window.enableSelfTest(args[i + 1], args[i + 2]);
        } else if (args[i] == "--uploadtest" && i + 1 < args.size()) {
            window.enableUploadTest(args[i + 1]);
        }
    }
    return app.exec();
}
