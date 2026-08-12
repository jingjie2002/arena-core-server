#pragma once

#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/core/result.hpp"

#include <functional>
#include <string>
#include <string_view>

#include "arena_core.pb.h"

namespace arena_core::session {

class BusinessService {
 public:
  using Completion = std::function<void(Result<protocol::v1::Envelope>)>;
  using PlayerNotifier = std::function<void(std::string_view, protocol::v1::Envelope)>;

  virtual ~BusinessService() = default;
  virtual void start() = 0;
  virtual void stop() = 0;
  virtual void set_player_notifier(PlayerNotifier notifier) = 0;
  virtual void handle(std::string player_id, protocol::v1::Envelope request, Completion completion) = 0;
};

}  // namespace arena_core::session
