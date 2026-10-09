#include <cstdlib>
#include <iostream>
#include <exception>

#ifdef _WIN32
#include <QDebug>

#include "debug_log.hpp"
#include "okuflow/app/app.hpp"
#endif

int main(int argc, char* argv[]) {
#ifdef _WIN32
    int exitCode = EXIT_FAILURE;
    try {
        okuflow::OkuFlowApp app(argc, argv);
        if (!app.Initialize()) {
            qCritical() << "OkuFlow initialization failed.";
        } else {
            exitCode = app.Run();
        }
    } catch (const std::exception& ex) {
        qCritical() << "Fatal error:" << ex.what();
    }
    okuflow::debug_log::Shutdown();
    return exitCode;
#else
    std::cerr << "OkuFlow currently supports Windows with CUDA, Qt, and Direct3D12 only.\n";
    return EXIT_FAILURE;
#endif
}
