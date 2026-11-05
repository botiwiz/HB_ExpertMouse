#include "cli.h"
#include <QGuiApplication>
#include <cstring>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);               // loads image format plugins
    if (argc > 1 && !strcmp(argv[1], "--convert")) return cli_convert(argc, argv);
    return cli_convert(1, argv);
}
