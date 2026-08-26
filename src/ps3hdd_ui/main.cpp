#include "main_window.h"

#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("UFS2Xplorer");
    QCoreApplication::setApplicationName("UFS2Xplorer");

    ps3hdd::ui::main_window w;
    w.show();
    return app.exec();
}