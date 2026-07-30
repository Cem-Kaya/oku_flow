#include <cstdlib>
#include <iostream>
#include <exception>

#ifdef _WIN32
#include <QDebug>

#include "debug_log.hpp"
#include "openzoom/app/app.hpp"
#endif

int main(int argc, char* argv[]) {
#ifdef _WIN32
    int exitCode = EXIT_FAILURE;
    try {
        openzoom::OpenZoomApp app(argc, argv);
        if (!app.Initialize()) {
            qCritical() << "OpenZoom initialization failed.";
        } else {
            exitCode = app.Run();
        }
    } catch (const std::exception& ex) {
        qCritical() << "Fatal error:" << ex.what();
    }
    openzoom::debug_log::Shutdown();
    return exitCode;
#else
    std::cerr << "OpenZoom currently supports Windows with CUDA, Qt, and Direct3D12 only.\n";
    return EXIT_FAILURE;
#endif
}
