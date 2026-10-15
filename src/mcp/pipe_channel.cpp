#include "okuflow/mcp/pipe_channel.hpp"
#include "okuflow/mcp/windows_pipe.hpp"
#include <condition_variable>
#include <deque>
#include <set>
#include <stdexcept>
#include <thread>

namespace okuflow::mcp {
namespace {
std::string GetString(const Json &j, std::string_view key) {
    auto p = j.Find(key);
    return p && p->IsString() ? p->String() : std::string();
}
std::string Decode(std::string_view s) {
    if (s.size() > 256 * 1024 || s.size() % 4)
        throw std::invalid_argument("invalid_image");
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < s.size(); i += 4) {
        unsigned n = 0;
        for (size_t j = 0; j < 4; ++j) {
            if (s[i + j] == '=') {
                if (i + 4 != s.size() || j < 2 || (j == 2 && s[i + 3] != '='))
                    throw std::invalid_argument("invalid_image");
                n <<= 6;
            } else {
                auto v = alphabet.find(s[i + j]);
                if (v == std::string_view::npos)
                    throw std::invalid_argument("invalid_image");
                n = (n << 6) | static_cast<unsigned>(v);
            }
        }
        out += static_cast<char>(n >> 16);
        if (s[i + 2] != '=')
            out += static_cast<char>(n >> 8);
        if (s[i + 3] != '=')
            out += static_cast<char>(n);
    }
    return out;
}
} // namespace
struct PipeChannel::Impl {
    struct Call {
        std::string key, tool;
        Json args;
        Done done;
        ULONGLONG deadline{};
    };
    std::string selector, secret{RandomHex(32)}, epoch{RandomHex()}, name{"MCP client"};
    agent::CatalogEdition edition;
    std::mutex mutex, writeMutex;
    std::condition_variable changed;
    std::deque<Call> calls;
    std::set<std::string> canceled;
    Handle pipe;
    std::shared_ptr<PeerIdentity> server;
    InstanceDescriptor descriptor;
    std::atomic_bool stop{};
    std::jthread worker;
    bool started{}, mismatch{};
    Impl(std::string s, agent::CatalogEdition e) : selector(std::move(s)), edition(e) {}
    bool Send(Json j) {
        std::lock_guard lock(writeMutex);
        return pipe && WriteFrame(pipe.value, j.Dump(), stop);
    }
    void Drop() {
        std::lock_guard lock(writeMutex);
        server.reset();
        pipe = {};
    }
    bool Connect() {
        if (secret.empty() || epoch.empty())
            return false;
        descriptor = InstanceDescriptor::Read(selector);
        if (descriptor.selector.empty())
            return false;
        Handle next = ConnectPipe(descriptor);
        if (!next)
            return false;
        auto image = CurrentImage().parent_path() / L"OkuFlowApp.exe";
#ifdef OKUFLOW_MCP_DEVELOPMENT_IMAGE
        image = CurrentImage().parent_path() / L"oku_flow.exe";
#endif
        auto identity = PeerIdentity::Verify(next.value, false, image);
        if (!identity)
            return false;
        std::string client;
        {
            std::lock_guard lock(mutex);
            client = name;
        }
        Json hello = Json::Object{{"type", "hello"},
                                  {"protocolVersion", ProtocolVersion},
                                  {"appEpoch", descriptor.appEpoch},
                                  {"pairingSecret", secret},
                                  {"mcpSessionEpoch", epoch},
                                  {"pipeEpoch", RandomHex()},
                                  {"clientName", client}};
        std::string bytes;
        if (!WriteFrame(next.value, hello.Dump(), stop) || !ReadFrame(next.value, bytes, stop, 2000))
            return false;
        try {
            auto j = Json::Parse(bytes);
            if (GetString(j, "type") != "hello" || GetString(j, "protocolVersion") != ProtocolVersion ||
                GetString(j, "appEpoch") != descriptor.appEpoch)
                return false;
            mismatch = GetString(j, "catalogEdition") != agent::CatalogEditionName(edition);
        } catch (...) {
            return false;
        }
        {
            std::lock_guard lock(writeMutex);
            pipe = std::move(next);
            server = std::move(identity);
        }
        return true;
    }
    void Run() {
        auto heartbeat = GetTickCount64();
        while (!stop.load()) {
            Call call;
            bool hasCall = false;
            {
                std::unique_lock lock(mutex);
                changed.wait_for(lock, std::chrono::milliseconds(100),
                                 [&] { return stop.load() || !calls.empty(); });
                if (stop.load())
                    break;
                if (!calls.empty()) {
                    call = std::move(calls.front());
                    calls.pop_front();
                    hasCall = true;
                }
            }
            if (hasCall && GetTickCount64() >= call.deadline) {
                call.done({Failure("timeout"), {}});
                continue;
            }
            if (!pipe && !Connect()) {
                if (hasCall)
                    call.done({Failure("unavailable"), {}});
                continue;
            }
            if (hasCall) {
                bool cancel;
                {
                    std::lock_guard lock(mutex);
                    cancel = canceled.erase(call.key) != 0;
                }
                if (cancel)
                    continue;
                if (mismatch) {
                    call.done({Failure("unavailable", "catalog_mismatch"), {}});
                    continue;
                }
                Json request = Json::Object{
                    {"type", "call"}, {"key", call.key}, {"tool", call.tool}, {"arguments", call.args}};
                bool sent = Send(request);
                std::string bytes;
                const auto remaining = [&]() -> DWORD {
                    const auto now = GetTickCount64();
                    return now < call.deadline ? static_cast<DWORD>(call.deadline - now) : 0;
                };
                bool got = sent && remaining() && ReadFrame(pipe.value, bytes, stop, remaining());
                if (!got && !stop.load()) {
                    Drop();
                    if (remaining() && Connect() && !mismatch && remaining() && Send(request) && remaining())
                        got = ReadFrame(pipe.value, bytes, stop, remaining());
                }
                ToolReply reply{Failure(remaining() ? "unavailable" : "timeout"), {}};
                if (got) {
                    try {
                        auto j = Json::Parse(bytes);
                        auto meta = j.Find("metadata");
                        if (GetString(j, "type") != "result" || GetString(j, "key") != call.key || !meta ||
                            !meta->IsObject())
                            throw std::invalid_argument("invalid_reply");
                        reply.metadata = *meta;
                        auto image = j.Find("jpeg");
                        if (image) {
                            if (!image->IsString())
                                throw std::invalid_argument("invalid_image");
                            reply.jpeg = Decode(image->String());
                        }
                    } catch (...) {
                        reply = {Failure("unavailable"), {}};
                        Drop();
                    }
                } else
                    Drop();
                bool canceledLater;
                {
                    std::lock_guard lock(mutex);
                    canceledLater = canceled.erase(call.key) != 0;
                }
                if (!canceledLater && !stop.load())
                    call.done(std::move(reply));
                heartbeat = GetTickCount64();
            } else if (GetTickCount64() - heartbeat >= 2000) {
                if (!Send(Json::Object{{"type", "heartbeat"}}))
                    Drop();
                heartbeat = GetTickCount64();
            }
        }
        Drop();
        std::deque<Call> pending;
        {
            std::lock_guard lock(mutex);
            pending.swap(calls);
        }
        for (auto &c : pending)
            c.done({Failure("canceled"), {}});
    }
};
PipeChannel::PipeChannel(std::string selector, agent::CatalogEdition edition)
    : impl_(std::make_unique<Impl>(std::move(selector), edition)) {}
PipeChannel::~PipeChannel() { Close(); }
void PipeChannel::SetClientName(std::string name) {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->name = SafeClientName(name);
        if (impl_->started)
            return;
        impl_->started = true;
    }
    impl_->worker = std::jthread([this] { impl_->Run(); });
}
void PipeChannel::Call(std::string key, std::string tool, Json args, Done done) {
    bool rejected = false;
    {
        std::lock_guard lock(impl_->mutex);
        rejected = impl_->stop.load() || impl_->calls.size() >= MaxPending;
        if (!rejected)
            impl_->calls.push_back(
                {std::move(key), std::move(tool), std::move(args), std::move(done), GetTickCount64() + 6500});
    }
    if (rejected)
        done({Failure("budget_exhausted"), {}});
    else
        impl_->changed.notify_one();
}
void PipeChannel::Cancel(std::string_view key) {
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->canceled.size() < MaxPending)
            impl_->canceled.insert(std::string(key));
    }
    impl_->Send(Json::Object{{"type", "cancel"}, {"key", key}});
}
void PipeChannel::Close() {
    if (!impl_ || impl_->stop.load())
        return;
    impl_->Send(Json::Object{{"type", "disconnect"}});
    impl_->stop = true;
    impl_->changed.notify_all();
    {
        std::lock_guard lock(impl_->writeMutex);
        if (impl_->pipe)
            CancelIoEx(impl_->pipe.value, nullptr);
    }
    if (impl_->worker.joinable())
        impl_->worker.join();
}
} // namespace okuflow::mcp
