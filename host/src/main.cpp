#include <QApplication>

#include "ui/mainwindow.h"

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("Timing Generator Host");
    QApplication::setOrganizationName("stm32-precision-timing-generator");

    MainWindow w;
    w.show();
    return app.exec();
}
