#include "okuflow/mcp/pipe_channel.hpp"
#include "okuflow/mcp/windows_pipe.hpp"
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <mutex>
#include <thread>

namespace {
bool WriteStdout(std::string line) {
    struct WriteState {
        std::string line;
        std::atomic_bool ok{};
    };
    auto state = std::make_shared<WriteState>();
    state->line = std::move(line);
    std::thread writer([state] {
        DWORD written{};
        state->ok = WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), state->line.data(),
                              static_cast<DWORD>(state->line.size()), &written, nullptr) &&
                    written == state->line.size();
    });
    if (WaitForSingleObject(writer.native_handle(), 2000) != WAIT_OBJECT_0) {
        CancelSynchronousIo(writer.native_handle());
        if (WaitForSingleObject(writer.native_handle(), 1000) != WAIT_OBJECT_0) {
            // The payload remains owned by the detached writer until process
            // exit; never destroy bytes still referenced by synchronous I/O.
            writer.detach();
            return false;
        }
    }
    writer.join();
    return state->ok.load();
}
} // namespace

int main(int argc, char **argv) {
    using namespace okuflow::mcp;
    if (argc == 2 && std::string_view(argv[1]) == "--build-info") {
        // Packaging-only diagnostics. No pipe, descriptor, consent or request
        // is opened, and stdout still contains one bounded JSON line.
#if defined(OKUFLOW_MCP_DEVELOPMENT_IMAGE)
        constexpr bool deployed = false;
#else
        constexpr bool deployed = true;
#endif
        const auto edition = okuflow::agent::CatalogEditionName(okuflow::agent::BuiltControlCatalogEdition());
        std::printf("{\"schema\":1,\"product\":\"OkuFlow MCP\",\"protocol\":\"2025-11-25\","
                    "\"catalogEdition\":\"%.*s\",\"deployedImagePolicy\":%s}\n",
                    static_cast<int>(edition.size()), edition.data(), deployed ? "true" : "false");
        return 0;
    }
    std::string selector;
    if (argc == 3 && std::string_view(argv[1]) == "--instance" && IsHex(argv[2], 32))
        selector = argv[2];
    else {
        std::fputs("configuration_invalid\n", stderr);
        return 2;
    }
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    std::mutex stdoutMutex;
    HANDLE mainRaw{};
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &mainRaw,
                         THREAD_TERMINATE | SYNCHRONIZE, FALSE, 0))
        return 2;
    Handle mainThread(mainRaw);
    const auto edition = okuflow::agent::BuiltControlCatalogEdition();
    PipeChannel channel(selector, edition);
    std::atomic_bool outputFailed{};
    Protocol protocol(
        channel,
        [&](std::string line) {
            std::lock_guard lock(stdoutMutex);
            line += '\n';
            if (!WriteStdout(std::move(line))) {
                outputFailed = true;
                CancelSynchronousIo(mainThread.value);
            }
        },
        edition);
    std::string line;
    line.reserve(4096);
    bool oversized = false;
    for (;;) {
        int ch = std::getchar();
        if (ch == EOF)
            break;
        if (ch == '\n') {
            bool open = oversized ? protocol.OversizedLine() : protocol.Line(line);
            line.clear();
            oversized = false;
            if (!open || outputFailed.load())
                break;
        } else if (!oversized) {
            if (line.size() >= MaxStdioLine - 1) {
                line.clear();
                oversized = true;
            } else
                line.push_back(static_cast<char>(ch));
        }
    }
    protocol.Close();
    return outputFailed.load() ? 1 : 0;
}
