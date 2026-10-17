#include "okuflow/mcp/app_ipc_endpoint.hpp"
#include "okuflow/mcp/protocol.hpp"
#include "okuflow/mcp/windows_pipe.hpp"
#include <QJsonDocument>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <deque>
#include <future>
#include <set>
#include <thread>

namespace okuflow::mcp {
namespace {
const std::string &ProcessEpoch() {
    static const auto epoch = RandomHex();
    return epoch;
}
Json ResultFrame(std::string_view key, const agent::ControlResult &result);
struct PipeSession : std::enable_shared_from_this<PipeSession> {
    Handle pipe;
    std::shared_ptr<PeerIdentity> identity;
    std::atomic_bool stopped{};
    std::mutex mutex;
    std::condition_variable ready;
    struct Outgoing {
        std::string bytes;
        std::function<bool()> fence;
        std::string deniedBytes;
        std::function<std::string()> serialize;
    };
    std::deque<Outgoing> outgoing;
    std::set<std::string> pending;
    std::set<std::string> canceled;
    std::string leaseContext;
    std::jthread writer;
    std::string secret, mcpEpoch, pipeEpoch, clientName;
    explicit PipeSession(Handle p) : pipe(std::move(p)) {}
    ~PipeSession() { Stop(); }
    void Start() {
        writer = std::jthread([this] {
            for (;;) {
                Outgoing frame;
                {
                    std::unique_lock lock(mutex);
                    ready.wait(lock, [&] { return stopped.load() || !outgoing.empty(); });
                    if (stopped.load())
                        break;
                    frame = std::move(outgoing.front());
                    outgoing.pop_front();
                }
                if (frame.serialize) {
                    try {
                        frame.bytes = frame.serialize();
                    } catch (...) {
                        frame.bytes = frame.deniedBytes;
                    }
                }
                if (stopped.load())
                    break;
                if (frame.fence && !frame.fence()) {
                    if (stopped.load())
                        break;
                    frame.bytes = std::move(frame.deniedBytes);
                }
                if (!WriteFrame(pipe.value, frame.bytes, stopped)) {
                    Stop();
                    break;
                }
            }
        });
    }
    void Stop() {
        stopped = true;
        ready.notify_all();
        CancelIoEx(pipe.value, nullptr);
    }
    void Send(Json value, std::function<bool()> fence = {}, std::string deniedBytes = {}) {
        auto bytes = value.Dump();
        std::lock_guard lock(mutex);
        if (stopped.load())
            return;
        if (bytes.size() > MaxIpcFrame || outgoing.size() >= MaxPending + 2) {
            stopped = true;
            ready.notify_all();
            return;
        }
        outgoing.push_back({std::move(bytes), std::move(fence), std::move(deniedBytes), {}});
        ready.notify_one();
    }
    void SendResult(std::string key, agent::ControlResult result, std::function<bool()> fence,
                    std::string denied) {
        std::lock_guard lock(mutex);
        if (stopped.load())
            return;
        if (outgoing.size() >= MaxPending + 2) {
            stopped = true;
            ready.notify_all();
            return;
        }
        auto serialize = [key = std::move(key), result = std::move(result)] {
            return ResultFrame(key, result).Dump();
        };
        outgoing.push_back({{}, std::move(fence), std::move(denied), std::move(serialize)});
        ready.notify_one();
    }
    bool Reserve(const std::string &key) {
        std::lock_guard lock(mutex);
        return pending.size() < MaxPending && pending.insert(key).second;
    }
    void SetLease(const QString &lease) {
        std::lock_guard lock(mutex);
        if (leaseContext != lease.toStdString())
            canceled.clear();
        leaseContext = lease.toStdString();
    }
    std::string Lease() {
        std::lock_guard lock(mutex);
        return leaseContext;
    }
    bool Cancel(const std::string &key) {
        std::lock_guard lock(mutex);
        if (canceled.size() < 128)
            canceled.insert(key);
        return pending.erase(key) != 0;
    }
    bool IsCanceled(const std::string &key) {
        std::lock_guard lock(mutex);
        return canceled.contains(key);
    }
    bool Retire(const std::string &key) {
        std::lock_guard lock(mutex);
        return pending.erase(key) != 0 && !stopped.load();
    }
};
std::string String(const Json &j, std::string_view key) {
    auto *p = j.Find(key);
    return p && p->IsString() ? p->String() : std::string();
}
Json ResultFrame(std::string_view key, const agent::ControlResult &result) {
    Json metadata;
    try {
        const auto bytes = QJsonDocument(result.content).toJson(QJsonDocument::Compact);
        metadata = Json::Parse(std::string_view(bytes.constData(), bytes.size()));
    } catch (...) {
        metadata = Failure("internal");
    }
    if (metadata.Dump().size() > MaxStructuredResult || result.jpeg.size() > static_cast<qsizetype>(MaxJpeg))
        return Json::Object{{"type", "result"}, {"key", key}, {"metadata", Failure("budget_exhausted")}};
    Json::Object frame{{"type", "result"}, {"key", key}, {"metadata", std::move(metadata)}};
    if (!result.jpeg.isEmpty())
        frame.emplace("jpeg", Base64(std::string_view(result.jpeg.constData(), result.jpeg.size())));
    return frame;
}
} // namespace
struct AppIpcEndpoint::Impl {
    AppIpcEndpoint &owner;
    agent::AppControlService &service;
    InstanceDescriptor descriptor;
    std::string appEpoch{ProcessEpoch()};
    quint64 leaseGeneration{};
    uint64_t maxRequestSequence{}, retiredRequestSequence{};
    std::atomic_bool stop{};
    std::jthread worker;
    QTimer watch;
    std::shared_ptr<PipeSession> session;
    std::shared_ptr<PeerIdentity> pairedIdentity;
    std::string pairedSecret, pairedMcpEpoch;
    QString clientName, context, peerId;
    std::function<void()> changed;
    bool enabled{};
#if defined(OKUFLOW_MCP_TEST_HOOKS)
    int fenceDelay{};
    std::function<void()> fencePosted;
#endif
    Impl(AppIpcEndpoint &o, agent::AppControlService &s) : owner(o), service(s) {
        watch.setInterval(250);
        QObject::connect(&watch, &QTimer::timeout, &owner, [this] {
            if (!context.isEmpty()) {
                auto status = service.ContextStatus();
                if (!status.value("active").toBool() || status.value("contextId").toString() != context ||
                    status.value("remainingAbsoluteMs").toDouble() <= 0 ||
                    status.value("remainingIdleMs").toDouble() <= 0)
                    Revoke();
            }
            if (pairedIdentity && !pairedIdentity->Alive()) {
                Revoke();
                pairedIdentity.reset();
                pairedSecret.clear();
                pairedMcpEpoch.clear();
                clientName.clear();
                peerId.clear();
                if (changed)
                    changed();
            }
        });
    }
    void Revoke() {
        if (!context.isEmpty()) {
            retiredRequestSequence = maxRequestSequence;
            if (session)
                session->SetLease({});
            service.End(context);
            context.clear();
        }
        if (changed)
            changed();
    }
    void Adopt(std::shared_ptr<PipeSession> next) {
        if (!enabled || stop.load() || next->stopped.load())
            return;
        if (!pairedIdentity || !pairedIdentity->SameProcess(*next->identity) ||
            pairedSecret != next->secret || pairedMcpEpoch != next->mcpEpoch) {
            Revoke();
            pairedIdentity = next->identity;
            pairedSecret = next->secret;
            pairedMcpEpoch = next->mcpEpoch;
            peerId = QString::fromStdString(RandomHex());
            maxRequestSequence = retiredRequestSequence = 0;
        }
        session = std::move(next);
        clientName = QString::fromStdString(session->clientName);
        session->SetLease(context);
        session->Send(Json::Object{{"type", "hello"},
                                   {"protocolVersion", ProtocolVersion},
                                   {"appEpoch", descriptor.appEpoch},
                                   {"catalogEdition", agent::CatalogEditionName(service.Edition())}});
        if (changed)
            changed();
    }
    void Dispatch(std::shared_ptr<PipeSession> s, std::string key, std::string tool,
                  std::string argumentBytes, std::string admittedLease) {
        if (!enabled || session != s || s->stopped.load() || context.isEmpty() || !s->identity->Alive()) {
            if (s->Retire(key))
                s->Send(Json::Object{{"type", "result"},
                                     {"key", key},
                                     {"metadata", Failure("unavailable", "lease_required")}});
            return;
        }
        if (QString::fromStdString(admittedLease) != context) {
            if (s->Retire(key))
                s->Send(
                    Json::Object{{"type", "result"}, {"key", key}, {"metadata", Failure("context_expired")}});
            return;
        }
        uint64_t sequence{};
        if (!key.starts_with("request-")) {
            s->Stop();
            return;
        }
        const auto parsed = std::from_chars(key.data() + 8, key.data() + key.size(), sequence);
        if (parsed.ec != std::errc{} || parsed.ptr != key.data() + key.size() || !sequence) {
            s->Stop();
            return;
        }
        if (sequence <= retiredRequestSequence) {
            if (s->Retire(key))
                s->Send(
                    Json::Object{{"type", "result"}, {"key", key}, {"metadata", Failure("context_expired")}});
            return;
        }
        maxRequestSequence = std::max(maxRequestSequence, sequence);
        auto args = QJsonDocument::fromJson(QByteArray::fromStdString(argumentBytes));
        if (!args.isObject()) {
            if (s->Retire(key))
                s->Send(Json::Object{
                    {"type", "result"}, {"key", key}, {"metadata", Failure("invalid_argument")}});
            return;
        }
        const QString lease = context;
        QPointer<AppIpcEndpoint> guarded(&owner);
        service.Submit(
            {lease, QString::fromStdString(key), QString::fromStdString(tool), args.object()},
            [guarded, s, key, lease](agent::ControlResult result) {
                if (!guarded || guarded->impl_->session != s || !s->identity->Alive()) {
                    s->Cancel(key);
                    return;
                }
                if (guarded->impl_->context != lease) {
                    if (s->Retire(key))
                        s->Send(Json::Object{
                            {"type", "result"}, {"key", key}, {"metadata", Failure("context_expired")}});
                    return;
                }
                if (!s->Retire(key))
                    return;
                std::function<bool()> fence;
                if (!result.jpeg.isEmpty() && !result.observation)
                    result = agent::ControlResult::Failure("stale_observation");
                if (!result.jpeg.isEmpty()) {
                    const auto expected = *result.observation;
                    int delay{};
                    std::function<void()> posted;
#if defined(OKUFLOW_MCP_TEST_HOOKS)
                    delay = guarded->impl_->fenceDelay;
                    posted = guarded->impl_->fencePosted;
#endif
                    fence = [guarded, s, lease, expected, key, delay, posted] {
                        if (s->stopped.load() || !guarded)
                            return false;
                        auto promise = std::make_shared<std::promise<bool>>();
                        auto ready = promise->get_future();
                        auto authorize = [guarded, s, lease, expected, key, promise] {
                            const bool permitted =
                                guarded && guarded->impl_->session == s && guarded->impl_->context == lease &&
                                !s->stopped.load() && !s->IsCanceled(key) && s->identity->Alive() &&
                                guarded->impl_->service.AuthorizeFrameDelivery(expected);
                            promise->set_value(permitted);
                        };
                        QMetaObject::invokeMethod(
                            guarded.data(),
                            [guarded, delay, authorize = std::move(authorize)] {
                                if (!guarded)
                                    return;
                                if (delay > 0)
                                    QTimer::singleShot(delay, guarded.data(), authorize);
                                else
                                    authorize();
                            },
                            Qt::QueuedConnection);
                        if (posted)
                            posted();
                        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                        while (!s->stopped.load() && std::chrono::steady_clock::now() < until) {
                            if (ready.wait_for(std::chrono::milliseconds(20)) == std::future_status::ready)
                                return ready.get();
                        }
                        return false;
                    };
                }
                const auto denied =
                    Json(Json::Object{
                             {"type", "result"}, {"key", key}, {"metadata", Failure("context_expired")}})
                        .Dump();
                s->SendResult(key, std::move(result), std::move(fence), denied);
            });
    }
    void Run(Handle first) {
        QPointer<AppIpcEndpoint> guarded(&owner);
        Handle pipe = std::move(first);
        while (!stop.load()) {
            if (!pipe)
                pipe = CreateSecurePipe(descriptor);
            if (!pipe || !AcceptPipe(pipe.value, stop))
                break;
            auto s = std::make_shared<PipeSession>(std::move(pipe));
            const auto reject = [&] {
                s->Stop();
                DisconnectNamedPipe(s->pipe.value);
                pipe = std::move(s->pipe);
            };
            s->identity =
                PeerIdentity::Verify(s->pipe.value, true, CurrentImage().parent_path() / L"okuflow_mcp.exe");
            std::string bytes;
            if (!s->identity || !ReadFrame(s->pipe.value, bytes, stop, 2000)) {
                reject();
                continue;
            }
            Json hello;
            try {
                hello = Json::Parse(bytes);
            } catch (...) {
                reject();
                continue;
            }
            s->secret = String(hello, "pairingSecret");
            s->mcpEpoch = String(hello, "mcpSessionEpoch");
            s->pipeEpoch = String(hello, "pipeEpoch");
            s->clientName = SafeClientName(String(hello, "clientName"));
            if (String(hello, "type") != "hello" || String(hello, "protocolVersion") != ProtocolVersion ||
                String(hello, "appEpoch") != descriptor.appEpoch || !IsHex(s->secret, 64) ||
                !IsHex(s->mcpEpoch, 32) || !IsHex(s->pipeEpoch, 32)) {
                reject();
                continue;
            }
            s->Start();
            QMetaObject::invokeMethod(
                &owner,
                [guarded, s] {
                    if (guarded)
                        guarded->impl_->Adopt(s);
                },
                Qt::QueuedConnection);
            while (!stop.load() && !s->stopped.load()) {
                if (!ReadFrame(s->pipe.value, bytes, stop, 8000))
                    break;
                Json request;
                try {
                    request = Json::Parse(bytes);
                } catch (...) {
                    break;
                }
                auto type = String(request, "type"), key = String(request, "key");
                if (type == "heartbeat")
                    continue;
                if (type == "disconnect") {
                    QMetaObject::invokeMethod(
                        &owner,
                        [guarded, s] {
                            if (guarded && guarded->impl_->session == s)
                                guarded->impl_->Revoke();
                        },
                        Qt::QueuedConnection);
                    break;
                }
                if (key.empty() || key.size() > 80)
                    break;
                if (type == "cancel") {
                    if (s->Cancel(key))
                        s->Send(Json::Object{
                            {"type", "result"}, {"key", key}, {"metadata", Failure("canceled")}});
                    QMetaObject::invokeMethod(
                        &owner,
                        [guarded, s, key] {
                            if (guarded && guarded->impl_->session == s && !guarded->impl_->context.isEmpty())
                                guarded->impl_->service.Cancel(guarded->impl_->context,
                                                               QString::fromStdString(key));
                        },
                        Qt::QueuedConnection);
                    continue;
                }
                if (type != "call")
                    break;
                auto tool = String(request, "tool");
                auto *args = request.Find("arguments");
                if (tool.empty() || !args || !args->IsObject() || args->Dump().size() > MaxArguments)
                    break;
                if (!s->Reserve(key)) {
                    s->Send(Json::Object{
                        {"type", "result"}, {"key", key}, {"metadata", Failure("budget_exhausted")}});
                    continue;
                }
                auto argumentBytes = args->Dump();
                auto admittedLease = s->Lease();
                QMetaObject::invokeMethod(
                    &owner,
                    [guarded, s, key, tool, argumentBytes, admittedLease] {
                        if (guarded)
                            guarded->impl_->Dispatch(s, key, tool, argumentBytes, admittedLease);
                    },
                    Qt::QueuedConnection);
            }
            s->Stop();
            s->writer.join();
            DisconnectNamedPipe(s->pipe.value);
            pipe = std::move(s->pipe);
            QMetaObject::invokeMethod(
                &owner,
                [guarded, s] {
                    if (guarded && guarded->impl_->session == s) {
                        guarded->impl_->session.reset();
                        if (guarded->impl_->changed)
                            guarded->impl_->changed();
                    }
                },
                Qt::QueuedConnection);
        }
    }
};
AppIpcEndpoint::AppIpcEndpoint(agent::AppControlService &service, QObject *parent)
    : QObject(parent), impl_(std::make_unique<Impl>(*this, service)) {}
AppIpcEndpoint::~AppIpcEndpoint() { SetEnabled(false); }
bool AppIpcEndpoint::SetEnabled(bool enabled) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (enabled == impl_->enabled)
        return true;
    if (!enabled) {
        impl_->enabled = false;
        impl_->stop = true;
        if (impl_->session)
            impl_->session->Stop();
        if (impl_->worker.joinable())
            impl_->worker.join();
        impl_->watch.stop();
        impl_->Revoke();
        impl_->session.reset();
        impl_->pairedIdentity.reset();
        impl_->pairedSecret.clear();
        impl_->pairedMcpEpoch.clear();
        impl_->clientName.clear();
        impl_->peerId.clear();
        impl_->descriptor.Remove();
        impl_->descriptor = {};
        return true;
    }
    impl_->descriptor = {RandomHex(), impl_->appEpoch};
    Handle pipe = CreateSecurePipe(impl_->descriptor);
    if (!pipe || !impl_->descriptor.Publish()) {
        impl_->descriptor.Remove();
        impl_->descriptor = {};
        return false;
    }
    impl_->stop = false;
    impl_->enabled = true;
    impl_->watch.start();
    impl_->worker = std::jthread([this, p = std::move(pipe)]() mutable { impl_->Run(std::move(p)); });
    return true;
}
QString AppIpcEndpoint::InstanceSelector() const {
    return QString::fromStdString(impl_->descriptor.selector);
}
QString AppIpcEndpoint::PendingClientName() const {
    return impl_->session && impl_->context.isEmpty() ? impl_->clientName : QString();
}
QString AppIpcEndpoint::ActiveLease() const { return impl_->context; }
QString AppIpcEndpoint::PendingPeerId() const {
    return !PendingClientName().isEmpty() ? impl_->peerId : QString();
}
bool AppIpcEndpoint::ApprovePending(const QString &expectedPeerId, const agent::ControlGrants &grants) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (expectedPeerId.isEmpty() || expectedPeerId != impl_->peerId || !impl_->enabled || !impl_->session ||
        impl_->session->stopped.load() || !impl_->context.isEmpty() || !impl_->pairedIdentity ||
        !impl_->pairedIdentity->Alive())
        return false;
    agent::ContextSpec spec;
    spec.kind = agent::ContextKind::ExternalLease;
    spec.owner = QString::fromStdString(RandomHex());
    if (spec.owner.isEmpty())
        return false;
    spec.epoch = ++impl_->leaseGeneration;
    spec.grants = grants;
    impl_->context = impl_->service.Begin(spec);
    impl_->session->SetLease(impl_->context);
    if (impl_->changed)
        impl_->changed();
    return !impl_->context.isEmpty();
}
void AppIpcEndpoint::Revoke() {
    Q_ASSERT(QThread::currentThread() == thread());
    impl_->Revoke();
}
void AppIpcEndpoint::SetPendingChanged(std::function<void()> cb) { impl_->changed = std::move(cb); }
#if defined(OKUFLOW_MCP_TEST_HOOKS)
void AppIpcEndpoint::SetFrameFenceTestHooks(int delayMs, std::function<void()> posted) {
    Q_ASSERT(QThread::currentThread() == thread());
    impl_->fenceDelay = std::clamp(delayMs, 0, 900);
    impl_->fencePosted = std::move(posted);
}
#endif
} // namespace okuflow::mcp
