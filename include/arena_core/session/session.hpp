#pragma once

#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/domain/session.hpp"
#include "arena_core/session/business_service.hpp"
#include "arena_core/session/session_registry.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/asio.hpp>

#include "arena_core.pb.h"

namespace arena_core::observer {
class ObserverStore;
}

namespace arena_core::session {

struct SessionOptions {
  std::size_t maximum_frame_bytes{65'536};
  domain::SessionStateMachine::Durations durations{};
  std::size_t request_cache_capacity{256};
  std::chrono::seconds request_cache_ttl{60};
  std::size_t maximum_pending_write_bytes{256 * 1024};
};

class Session final : public std::enable_shared_from_this<Session> {
 public:
  using ClosedCallback = std::function<void()>;

  Session(boost::asio::ip::tcp::socket socket,
          SessionRegistry& registry,
          SessionOptions options,
          std::shared_ptr<BusinessService> business_service,
          ClosedCallback closed_callback,
          std::shared_ptr<observer::ObserverStore> observer_store = {},
          std::string observer_session_id = {});

  void start();
  void request_stop();
  void push(protocol::v1::Envelope envelope);

 private:
  struct PendingWrite {
    std::shared_ptr<std::string> bytes;
    bool close_after{false};
  };

  struct CachedResponse {
    std::uint64_t fingerprint{0};
    std::shared_ptr<std::string> bytes;
    domain::TimePoint stored_at{};
  };

  void do_read_header();
  void do_read_body(std::size_t body_size);
  void handle_envelope(protocol::v1::Envelope envelope);
  void handle_hello(const protocol::v1::Envelope& envelope, std::uint64_t fingerprint);
  void handle_heartbeat(const protocol::v1::Envelope& envelope, std::uint64_t fingerprint);
  void handle_unimplemented(const protocol::v1::Envelope& envelope, std::uint64_t fingerprint);
  void handle_business(protocol::v1::Envelope envelope, std::uint64_t fingerprint);

  void send_response(protocol::v1::Envelope response,
                     std::uint64_t request_fingerprint,
                     bool remember,
                     bool close_after = false);
  void send_error(Error error,
                  std::uint64_t request_id,
                  std::uint64_t request_fingerprint,
                  bool remember,
                  bool force_close = false);
  void enqueue(std::shared_ptr<std::string> frame, bool close_after);
  void do_write();

  bool resend_if_cached(std::uint64_t request_id, std::uint64_t fingerprint);
  void remember(std::uint64_t request_id,
                std::uint64_t fingerprint,
                std::shared_ptr<std::string> response,
                domain::TimePoint now);
  void prune_cache(domain::TimePoint now);

  void arm_deadline();
  void handle_transport_failure();
  void final_stop();
  void close_socket() noexcept;
  std::uint64_t now_epoch_milliseconds() const;

  boost::asio::ip::tcp::socket socket_;
  boost::asio::strand<boost::asio::any_io_executor> strand_;
  boost::asio::steady_timer deadline_timer_;
  SessionRegistry& registry_;
  SessionOptions options_;
  std::shared_ptr<BusinessService> business_service_;
  ClosedCallback closed_callback_;
  std::shared_ptr<observer::ObserverStore> observer_store_;
  std::string observer_session_id_;
  domain::SessionStateMachine state_;

  std::array<std::uint8_t, 4> read_header_{};
  std::vector<std::uint8_t> read_body_;
  std::deque<PendingWrite> write_queue_;
  std::size_t pending_write_bytes_{0};
  bool write_in_progress_{false};
  bool closing_after_write_{false};
  bool stopped_{false};
  bool close_callback_sent_{false};

  std::string session_token_;
  std::string reconnect_token_;
  std::string player_id_;
  std::uint64_t generation_{0};

  std::unordered_map<std::uint64_t, CachedResponse> response_cache_;
  std::deque<std::uint64_t> response_order_;
  struct InflightRequest {
    std::uint64_t fingerprint{0};
    std::size_t duplicate_count{0};
  };
  std::unordered_map<std::uint64_t, InflightRequest> inflight_requests_;
};

}  // namespace arena_core::session
