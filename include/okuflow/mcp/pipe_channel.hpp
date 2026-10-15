#pragma once
#include "okuflow/mcp/protocol.hpp"
#include <memory>

namespace okuflow::mcp {
class PipeChannel final : public AppChannel {
  public:
    explicit PipeChannel(std::string selector,
                         agent::CatalogEdition edition = agent::BuiltControlCatalogEdition());
    ~PipeChannel() override;
    void SetClientName(std::string name) override;
    void Call(std::string key, std::string tool, Json arguments, Done done) override;
    void Cancel(std::string_view key) override;
    void Close() override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace okuflow::mcp
