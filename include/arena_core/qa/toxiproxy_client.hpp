#pragma once

#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/core/error.hpp"

#include <cstdint>
#include <string>

namespace arena_core::qa {

struct ToxiproxyClientConfig {
  std::string host{"127.0.0.1"};
  std::uint16_t api_port{8474};
  std::string proxy_name{"arena-core-lab-redis"};
  std::string listen{"0.0.0.0:26379"};
  std::string upstream{"redis:6379"};
  std::uint32_t request_timeout_ms{2'000};
};

enum class ToxiproxyFaultKind {
  kDisconnect,
  kLatency,
  kTimeout,
  kResetPeer,
};

class ToxiproxyClient final {
 public:
  using Config = ToxiproxyClientConfig;
  using FaultKind = ToxiproxyFaultKind;

  explicit ToxiproxyClient(ToxiproxyClientConfig config);

  Error probe() const;
  Error prepare() const;
  Error inject(ToxiproxyFaultKind kind, std::uint32_t value_ms = 0) const;
  Error reset() const;

 private:
  ToxiproxyClientConfig config_;
};

}  // namespace arena_core::qa
