#include "okuflow/mcp/protocol.hpp"
#include <cmath>

namespace okuflow::mcp {
namespace {
Json RpcError(Json id, int code, std::string_view safe) {
    return Json::Object{
        {"jsonrpc", "2.0"},
        {"id", std::move(id)},
        {"error", Json::Object{{"code", code}, {"message", "Request rejected"}, {"data", safe}}}};
}
Json RpcResult(Json id, Json result) {
    return Json::Object{{"jsonrpc", "2.0"}, {"id", std::move(id)}, {"result", std::move(result)}};
}
bool Id(const Json *p) {
    return p && ((p->IsString() && p->String().size() <= 256) ||
                 (p->IsNumber() && std::floor(p->Number()) == p->Number() &&
                  std::abs(p->Number()) <= 9007199254740991.0));
}
Json List(agent::CatalogEdition edition) {
    Json::Array tools;
    for (const auto &t : agent::ControlRegistry(edition)) {
        tools.emplace_back(Json::Object{{"name", t.canonicalId},
                                        {"description", t.description},
                                        {"inputSchema", Json::Parse(t.inputSchema)},
                                        {"outputSchema", Json::Parse(t.outputSchema)},
                                        {"annotations", Json::Object{{"readOnlyHint", t.readOnly},
                                                                     {"destructiveHint", agent::ControlToolDestructiveHint(t.id)},
                                                                     {"idempotentHint", t.idempotent},
                                                                     {"openWorldHint", false}}}});
    }
    return Json::Object{{"tools", std::move(tools)}};
}
Json Envelope(ToolReply reply) {
    bool error = true;
    if (auto ok = reply.metadata.Find("ok"); ok && ok->IsBool())
        error = !ok->Boolean();
    if (!reply.metadata.IsObject() || reply.metadata.Dump().size() > MaxStructuredResult ||
        reply.jpeg.size() > MaxJpeg) {
        reply = {Failure("budget_exhausted"), {}};
        error = true;
    }
    Json::Array content{Json::Object{{"type", "text"}, {"text", reply.metadata.Dump()}}};
    if (!error && !reply.jpeg.empty())
        content.emplace_back(
            Json::Object{{"type", "image"}, {"mimeType", "image/jpeg"}, {"data", Base64(reply.jpeg)}});
    Json::Object result{{"content", std::move(content)}, {"isError", error}};
    if (!error)
        result.emplace("structuredContent", std::move(reply.metadata));
    return result;
}
} // namespace
Json Failure(std::string_view code, std::string_view reason) {
    Json::Object error{{"code", code}, {"retryable", false}};
    if (!reason.empty())
        error.emplace("reason", reason);
    return Json::Object{{"ok", false}, {"error", std::move(error)}};
}
Protocol::Protocol(AppChannel &channel, Emit writeResponse, agent::CatalogEdition edition)
    : channel_(channel), edition_(edition), state_(std::make_shared<State>()) {
    state_->writeResponse = std::move(writeResponse);
}
Protocol::~Protocol() { Close(); }
void Protocol::Error(const Json &id, int code, std::string_view safe) {
    std::lock_guard lock(state_->mutex);
    if (state_->open)
        state_->writeResponse(RpcError(id, code, safe).Dump());
}
void Protocol::Result(const Json &id, Json result) {
    std::lock_guard lock(state_->mutex);
    if (state_->open)
        state_->writeResponse(RpcResult(id, std::move(result)).Dump());
}
bool Protocol::Strike() {
    std::lock_guard lock(state_->mutex);
    if (!state_->open)
        return false;
    state_->writeResponse(RpcError({}, -32700, "parse_error").Dump());
    if (++state_->strikes >= 3)
        state_->open = false;
    return state_->open;
}
bool Protocol::OversizedLine() { return Strike(); }
void Protocol::Close() {
    bool close = false;
    {
        std::lock_guard lock(state_->mutex);
        close = !state_->channelClosed;
        state_->channelClosed = true;
        state_->open = false;
        state_->pending.clear();
    }
    if (close)
        channel_.Close();
}
bool Protocol::Line(std::string_view line) {
    if (line.size() + 1 > MaxStdioLine)
        return OversizedLine();
    Json message;
    try {
        message = Json::Parse(line);
    } catch (...) {
        return Strike();
    }
    const auto *id = message.Find("id");
    const bool notification = !id;
    auto invalid = [&](int code, std::string_view why) {
        if (!notification || !message.IsObject())
            Error(Id(id) ? *id : Json(), code, why);
    };
    if (!message.IsObject()) {
        Error({}, -32600, "invalid_request");
        return true;
    }
    auto version = message.Find("jsonrpc"), method = message.Find("method"), params = message.Find("params");
    if (!version || !version->IsString() || version->String() != "2.0" || !method || !method->IsString() ||
        (!notification && !Id(id))) {
        invalid(-32600, "invalid_request");
        return true;
    }
    if (!notification && method->String() == "initialize") {
        bool duplicate;
        {
            std::lock_guard lock(state_->mutex);
            duplicate = state_->initializing;
        }
        if (duplicate) {
            Error(*id, -32600, "duplicate_initialize");
            return true;
        }
    }
    if (params && !params->IsObject()) {
        invalid(-32602, "invalid_params");
        return true;
    }
    Json empty(Json::Object{});
    const Json &p = params ? *params : empty;
    std::string m = method->String();
    if (notification) {
        if (m == "notifications/initialized") {
            std::lock_guard lock(state_->mutex);
            if (state_->initializing)
                state_->initialized = true;
        } else if (m == "notifications/cancelled") {
            auto rid = p.Find("requestId");
            if (Id(rid)) {
                std::string key;
                {
                    std::lock_guard lock(state_->mutex);
                    auto it = state_->pending.find(rid->Dump());
                    if (it != state_->pending.end()) {
                        key = it->second.key;
                        state_->pending.erase(it);
                    }
                }
                if (!key.empty())
                    channel_.Cancel(key);
            }
        }
        return true;
    }
    {
        std::lock_guard lock(state_->mutex);
        if (!state_->open)
            return false;
        if (state_->pending.contains(id->Dump())) {
            state_->writeResponse(RpcError(*id, -32600, "duplicate_id").Dump());
            return true;
        }
    }
    if (m == "initialize") {
        auto protocol = p.Find("protocolVersion"), cap = p.Find("capabilities"), info = p.Find("clientInfo");
        if (!protocol || !protocol->IsString() || !cap || !cap->IsObject() || !info || !info->IsObject()) {
            Error(*id, -32602, "invalid_params");
            return true;
        }
        const auto name = info->Find("name"), clientVersion = info->Find("version");
        if (!name || !name->IsString() || !clientVersion || !clientVersion->IsString()) {
            Error(*id, -32602, "invalid_params");
            return true;
        }
        for (auto capability : {"roots", "sampling", "elicitation", "tasks", "experimental"}) {
            const auto advertised = cap->Find(capability);
            if (advertised && !advertised->IsObject()) {
                Error(*id, -32602, "invalid_params");
                return true;
            }
        }
        bool duplicate;
        {
            std::lock_guard lock(state_->mutex);
            duplicate = state_->initializing;
            if (!duplicate)
                state_->initializing = true;
        }
        if (duplicate) {
            Error(*id, -32600, "duplicate_initialize");
            return true;
        }
        channel_.SetClientName(SafeClientName(name->String()));
        Result(*id,
               Json::Object{{"protocolVersion", ProtocolVersion},
                            {"capabilities", Json::Object{{"tools", Json::Object{{"listChanged", false}}}}},
                            {"serverInfo", Json::Object{{"name", "OkuFlow"},
                                                        {"version", agent::CatalogEditionName(edition_)}}}});
        return true;
    }
    bool started, ready;
    {
        std::lock_guard lock(state_->mutex);
        started = state_->initializing;
        ready = state_->initialized;
    }
    if (m == "ping") {
        if (!started)
            Error(*id, -32002, "not_initialized");
        else
            Result(*id, Json::Object{});
        return true;
    }
    if (m != "tools/list" && m != "tools/call") {
        Error(*id, -32601, "method_not_found");
        return true;
    }
    if (!ready) {
        Error(*id, -32002, "not_initialized");
        return true;
    }
    if (m == "tools/list") {
        if (p.Find("cursor")) {
            Error(*id, -32602, "invalid_cursor");
            return true;
        }
        Result(*id, List(edition_));
        return true;
    }
    auto name = p.Find("name"), args = p.Find("arguments");
    if (p.Find("task") || !name || !name->IsString()) {
        Error(*id, -32602, "invalid_params");
        return true;
    }
    const auto *tool = agent::FindControlTool(name->String(), edition_);
    if (!tool || tool->canonicalId != name->String()) {
        Error(*id, -32602, "unknown_tool");
        return true;
    }
    if (!args)
        args = &empty;
    if (!args->IsObject() || args->Dump().size() > MaxArguments ||
        !MatchesSchema(*args, Json::Parse(tool->inputSchema))) {
        Result(*id, Envelope({Failure("invalid_argument"), {}}));
        return true;
    }
    std::string requestKey, idKey = id->Dump();
    {
        std::lock_guard lock(state_->mutex);
        if (state_->pending.size() >= MaxPending) {
            state_->writeResponse(RpcResult(*id, Envelope({Failure("budget_exhausted"), {}})).Dump());
            return true;
        }
        requestKey = "request-" + std::to_string(++state_->nextKey);
        state_->pending.emplace(idKey, Pending{*id, requestKey});
    }
    std::weak_ptr<State> weak = state_;
    channel_.Call(requestKey, std::string(tool->canonicalId), *args,
                  [weak, idKey, requestKey, outputSchema = tool->outputSchema](ToolReply reply) {
                      auto state = weak.lock();
                      if (!state)
                          return;
                      Json result;
                      try {
                          const auto ok = reply.metadata.Find("ok");
                          if (ok && ok->IsBool() && ok->Boolean() &&
                              !MatchesSchema(reply.metadata, Json::Parse(outputSchema)))
                              reply = {Failure("internal"), {}};
                          result = Envelope(std::move(reply));
                      } catch (...) {
                          result = Envelope({Failure("internal"), {}});
                      }
                      std::lock_guard lock(state->mutex);
                      auto it = state->pending.find(idKey);
                      if (!state->open || it == state->pending.end() || it->second.key != requestKey)
                          return;
                      auto line = RpcResult(it->second.id, std::move(result)).Dump();
                      if (line.size() + 1 > MaxStdioLine)
                          line = RpcResult(it->second.id, Envelope({Failure("budget_exhausted"), {}})).Dump();
                      state->pending.erase(it);
                      state->writeResponse(std::move(line));
                  });
    return true;
}
} // namespace okuflow::mcp
