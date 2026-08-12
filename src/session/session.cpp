#include "arena_core/session/session.hpp"

#include "arena_core/protocol/frame.hpp"
#include "arena_core/observer/observer_store.hpp"

#include <chrono>
#include <utility>

#include <spdlog/spdlog.h>

namespace arena_core::session {
namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
namespace wire = arena_core::protocol::v1;

wire::Envelope make_response_envelope(const wire::MessageType type, const std::uint64_t request_id) {
  wire::Envelope response;
  response.set_protocol_version(1);
  response.set_message_type(type);
  response.set_request_id(request_id);
  return response;
}

}  // namespace

Session::Session(tcp::socket socket,
                 SessionRegistry& registry,
                 SessionOptions options,
                 std::shared_ptr<BusinessService> business_service,
                 ClosedCallback closed_callback,
                 std::shared_ptr<observer::ObserverStore> observer_store,
                 std::string observer_session_id)
    : socket_(std::move(socket)),
      strand_(asio::make_strand(socket_.get_executor())),
      deadline_timer_(strand_),
      registry_(registry),
      options_(std::move(options)),
      business_service_(std::move(business_service)),
      closed_callback_(std::move(closed_callback)),
      observer_store_(std::move(observer_store)),
      observer_session_id_(std::move(observer_session_id)),
      state_(domain::MonotonicClock::now(), options_.durations) {}

void Session::start() {
  asio::dispatch(strand_, [self = shared_from_this()] {
    if (self->observer_store_) {
      self->observer_store_->upsert_session(
          {self->observer_session_id_, "", "Connected", self->observer_session_id_, 0,
           self->now_epoch_milliseconds()});
      self->observer_store_->publish_event("session", "session.connected",
                                           self->observer_session_id_, self->observer_session_id_,
                                           "TCP connection accepted");
    }
    self->arm_deadline();
    self->do_read_header();
  });
}

void Session::request_stop() {
  asio::dispatch(strand_, [self = shared_from_this()] { self->final_stop(); });
}

void Session::push(wire::Envelope envelope) {
  asio::dispatch(strand_, [self = shared_from_this(), envelope = std::move(envelope)]() mutable {
    if (self->stopped_ || self->state_.state() != domain::SessionState::kActive) {
      return;
    }
    envelope.set_protocol_version(1);
    envelope.set_request_id(0);
    envelope.clear_session_token();
    self->send_response(std::move(envelope), 0, false);
  });
}

void Session::do_read_header() {
  if (stopped_ || closing_after_write_) {
    return;
  }
  asio::async_read(socket_, asio::buffer(read_header_), asio::bind_executor(strand_,
      [self = shared_from_this()](const boost::system::error_code& error, const std::size_t) {
        if (error) {
          self->handle_transport_failure();
          return;
        }
        const auto length = protocol::decode_frame_length(self->read_header_, self->options_.maximum_frame_bytes);
        if (!length.ok()) {
          self->send_error(length.error, 0, 0, false, true);
          return;
        }
        self->do_read_body(*length.value);
      }));
}

void Session::do_read_body(const std::size_t body_size) {
  read_body_.assign(body_size, 0);
  asio::async_read(socket_, asio::buffer(read_body_), asio::bind_executor(strand_,
      [self = shared_from_this()](const boost::system::error_code& error, const std::size_t) {
        if (error) {
          self->handle_transport_failure();
          return;
        }
        const auto payload = std::string_view(reinterpret_cast<const char*>(self->read_body_.data()),
                                              self->read_body_.size());
        auto parsed = protocol::parse_envelope(payload);
        if (!parsed.ok()) {
          self->send_error(parsed.error, 0, 0, false, true);
          return;
        }
        self->handle_envelope(std::move(*parsed.value));
        if (!self->stopped_ && !self->closing_after_write_) {
          self->do_read_header();
        }
      }));
}

void Session::handle_envelope(wire::Envelope envelope) {
  const auto fingerprint = protocol::request_fingerprint(envelope);
  if (envelope.protocol_version() != 1U) {
    send_error(make_error(ErrorCode::kProtocolUnsupportedVersion), envelope.request_id(), fingerprint, false, true);
    return;
  }
  if (!wire::MessageType_IsValid(envelope.message_type()) ||
      envelope.message_type() == wire::MESSAGE_TYPE_UNSPECIFIED) {
    send_error(make_error(ErrorCode::kProtocolUnknownMessage), envelope.request_id(), fingerprint, false, true);
    return;
  }
  if (envelope.request_id() == 0U) {
    send_error(make_error(ErrorCode::kProtocolDecodeFailed, "client request id must be non-zero"), 0, fingerprint,
               false, true);
    return;
  }
  if (resend_if_cached(envelope.request_id(), fingerprint)) {
    return;
  }

  if (envelope.message_type() == wire::HELLO) {
    handle_hello(envelope, fingerprint);
    return;
  }
  if (state_.state() != domain::SessionState::kActive) {
    send_error(make_error(ErrorCode::kSessionHelloRequired), envelope.request_id(), fingerprint, true, true);
    return;
  }
  if (envelope.session_token() != session_token_) {
    send_error(make_error(ErrorCode::kSessionTokenInvalid), envelope.request_id(), fingerprint, true, true);
    return;
  }
  if (envelope.message_type() == wire::HEARTBEAT) {
    handle_heartbeat(envelope, fingerprint);
    return;
  }
  handle_unimplemented(envelope, fingerprint);
}

void Session::handle_hello(const wire::Envelope& envelope, const std::uint64_t fingerprint) {
  if (state_.state() != domain::SessionState::kConnected) {
    send_error(make_error(ErrorCode::kSessionNotActive, "Hello already completed"), envelope.request_id(),
               fingerprint, true);
    return;
  }
  auto hello = protocol::parse_payload<wire::Hello>(envelope);
  if (!hello.ok()) {
    send_error(hello.error, envelope.request_id(), fingerprint, true, true);
    return;
  }

  const auto now = domain::MonotonicClock::now();
  Result<Authentication> authentication = hello.value->reconnect_token().empty()
      ? registry_.open_new(hello.value->player_id(), now)
      : registry_.resume(hello.value->player_id(), hello.value->reconnect_token(), now);
  if (!authentication.ok()) {
    const bool close = authentication.error.code == ErrorCode::kSessionTokenInvalid;
    send_error(authentication.error, envelope.request_id(), fingerprint, true, close);
    return;
  }
  if (auto error = state_.activate(hello.value->player_id(), now)) {
    registry_.revoke(authentication.value->reconnect_token, authentication.value->generation);
    send_error(std::move(error), envelope.request_id(), fingerprint, true, true);
    return;
  }

  player_id_ = hello.value->player_id();
  session_token_ = authentication.value->session_token;
  reconnect_token_ = authentication.value->reconnect_token;
  generation_ = authentication.value->generation;
  registry_.bind_transport(reconnect_token_, generation_, shared_from_this());
  if (observer_store_) {
    observer_store_->upsert_session({observer_session_id_, player_id_, "Active",
                                     observer_session_id_, generation_, now_epoch_milliseconds()});
    observer_store_->publish_event("session", "session.active", observer_session_id_,
                                   observer_session_id_, player_id_ + " completed Hello");
  }

  wire::HelloAck ack;
  ack.set_session_token(session_token_);
  ack.set_reconnect_token(reconnect_token_);
  ack.set_resumed(authentication.value->resumed);
  ack.set_generation(generation_);
  auto response = make_response_envelope(wire::HELLO_ACK, envelope.request_id());
  protocol::set_payload(response, ack);
  send_response(std::move(response), fingerprint, true);
  arm_deadline();
}

void Session::handle_heartbeat(const wire::Envelope& envelope, const std::uint64_t fingerprint) {
  auto heartbeat = protocol::parse_payload<wire::Heartbeat>(envelope);
  if (!heartbeat.ok()) {
    send_error(heartbeat.error, envelope.request_id(), fingerprint, true, true);
    return;
  }
  if (auto error = state_.record_activity(domain::MonotonicClock::now())) {
    send_error(std::move(error), envelope.request_id(), fingerprint, true, true);
    return;
  }
  if (observer_store_) {
    observer_store_->upsert_session({observer_session_id_, player_id_, "Active",
                                     observer_session_id_, generation_, now_epoch_milliseconds()});
  }

  wire::HeartbeatAck ack;
  ack.set_server_time_ms(now_epoch_milliseconds());
  auto response = make_response_envelope(wire::HEARTBEAT_ACK, envelope.request_id());
  protocol::set_payload(response, ack);
  send_response(std::move(response), fingerprint, true);
  arm_deadline();
}

void Session::handle_unimplemented(const wire::Envelope& envelope, const std::uint64_t fingerprint) {
  if (business_service_) {
    handle_business(envelope, fingerprint);
    return;
  }
  send_error(make_error(ErrorCode::kServerBusy, "message handler is not active before its milestone"),
             envelope.request_id(), fingerprint, true);
}

void Session::handle_business(wire::Envelope envelope, const std::uint64_t fingerprint) {
  if (auto error = state_.record_activity(domain::MonotonicClock::now())) {
    send_error(std::move(error), envelope.request_id(), fingerprint, true, true);
    return;
  }
  arm_deadline();
  const auto request_id = envelope.request_id();
  if (const auto inflight = inflight_requests_.find(request_id); inflight != inflight_requests_.end()) {
    if (inflight->second.fingerprint != fingerprint) {
      send_error(make_error(ErrorCode::kProtocolRequestIdConflict), request_id, fingerprint, false, true);
    } else {
      ++inflight->second.duplicate_count;
    }
    return;
  }
  inflight_requests_.emplace(request_id, InflightRequest{fingerprint, 0});
  std::weak_ptr<Session> weak_self = shared_from_this();
  business_service_->handle(player_id_, std::move(envelope),
      [weak_self, request_id, fingerprint](Result<wire::Envelope> result) mutable {
        if (const auto self = weak_self.lock()) {
          asio::dispatch(self->strand_, [self, request_id, fingerprint, result = std::move(result)]() mutable {
            const auto inflight = self->inflight_requests_.find(request_id);
            if (inflight == self->inflight_requests_.end() || self->stopped_ || self->closing_after_write_) {
              if (inflight != self->inflight_requests_.end()) {
                self->inflight_requests_.erase(inflight);
              }
              return;
            }
            const auto duplicate_count = inflight->second.duplicate_count;
            self->inflight_requests_.erase(inflight);
            if (result.ok()) {
              result.value->set_request_id(request_id);
              self->send_response(std::move(*result.value), fingerprint, true);
            } else {
              self->send_error(std::move(result.error), request_id, fingerprint, true);
            }
            const auto cached = self->response_cache_.find(request_id);
            if (cached != self->response_cache_.end()) {
              for (std::size_t index = 0; index < duplicate_count; ++index) {
                self->enqueue(cached->second.bytes, false);
              }
            }
          });
        }
      });
}

void Session::send_response(wire::Envelope response,
                            const std::uint64_t request_fingerprint,
                            const bool remember_response,
                            const bool close_after) {
  auto framed = protocol::frame_message(response, options_.maximum_frame_bytes);
  if (!framed.ok()) {
    final_stop();
    return;
  }
  auto bytes = std::make_shared<std::string>(std::move(*framed.value));
  if (remember_response) {
    remember(response.request_id(), request_fingerprint, bytes, domain::MonotonicClock::now());
  }
  enqueue(std::move(bytes), close_after);
}

void Session::send_error(Error error,
                         const std::uint64_t request_id,
                         const std::uint64_t request_fingerprint,
                         const bool remember_response,
                         const bool force_close) {
  if (observer_store_) {
    observer_store_->publish_event("session", "session.rejected", observer_session_id_,
                                   observer_session_id_, std::string(error_code_name(error.code)), "error");
  }
  wire::ErrorResponse payload;
  payload.set_code(std::string(error_code_name(error.code)));
  payload.set_message(std::move(error.message));
  payload.set_retryable(error.retryable);
  auto response = make_response_envelope(wire::ERROR_RESPONSE, request_id);
  protocol::set_payload(response, payload);
  send_response(std::move(response), request_fingerprint, remember_response,
                force_close || error.close_connection);
}

void Session::enqueue(std::shared_ptr<std::string> frame, const bool close_after) {
  if (stopped_) {
    return;
  }
  if (pending_write_bytes_ + frame->size() > options_.maximum_pending_write_bytes) {
    final_stop();
    return;
  }
  pending_write_bytes_ += frame->size();
  write_queue_.push_back(PendingWrite{std::move(frame), close_after});
  closing_after_write_ = closing_after_write_ || close_after;
  if (!write_in_progress_) {
    do_write();
  }
}

void Session::do_write() {
  if (stopped_ || write_queue_.empty()) {
    write_in_progress_ = false;
    return;
  }
  write_in_progress_ = true;
  const auto pending = write_queue_.front();
  asio::async_write(socket_, asio::buffer(*pending.bytes), asio::bind_executor(strand_,
      [self = shared_from_this(), pending](const boost::system::error_code& error, const std::size_t) {
        if (error) {
          self->handle_transport_failure();
          return;
        }
        self->pending_write_bytes_ -= pending.bytes->size();
        self->write_queue_.pop_front();
        if (pending.close_after) {
          self->final_stop();
          return;
        }
        self->do_write();
      }));
}

bool Session::resend_if_cached(const std::uint64_t request_id, const std::uint64_t fingerprint) {
  prune_cache(domain::MonotonicClock::now());
  const auto cached = response_cache_.find(request_id);
  if (cached == response_cache_.end()) {
    return false;
  }
  if (cached->second.fingerprint != fingerprint) {
    send_error(make_error(ErrorCode::kProtocolRequestIdConflict), request_id, fingerprint, false, true);
    return true;
  }
  enqueue(cached->second.bytes, false);
  return true;
}

void Session::remember(const std::uint64_t request_id,
                       const std::uint64_t fingerprint,
                       std::shared_ptr<std::string> response,
                       const domain::TimePoint now) {
  if (options_.request_cache_capacity == 0U) {
    return;
  }
  response_cache_[request_id] = CachedResponse{fingerprint, std::move(response), now};
  response_order_.push_back(request_id);
  prune_cache(now);
  while (response_order_.size() > options_.request_cache_capacity) {
    const auto oldest = response_order_.front();
    response_order_.pop_front();
    response_cache_.erase(oldest);
  }
}

void Session::prune_cache(const domain::TimePoint now) {
  while (!response_order_.empty()) {
    const auto request_id = response_order_.front();
    const auto cached = response_cache_.find(request_id);
    if (cached == response_cache_.end()) {
      response_order_.pop_front();
      continue;
    }
    if (now - cached->second.stored_at < options_.request_cache_ttl) {
      break;
    }
    response_cache_.erase(cached);
    response_order_.pop_front();
  }
}

void Session::arm_deadline() {
  if (stopped_) {
    return;
  }
  deadline_timer_.expires_at(state_.deadline());
  deadline_timer_.async_wait(asio::bind_executor(strand_,
      [self = shared_from_this()](const boost::system::error_code& error) {
        if (error == asio::error::operation_aborted || self->stopped_) {
          return;
        }
        if (self->state_.expire_if_due(domain::MonotonicClock::now())) {
          self->final_stop();
        }
      }));
}

void Session::handle_transport_failure() {
  if (stopped_) {
    return;
  }
  deadline_timer_.cancel();
  if (state_.state() == domain::SessionState::kActive && !reconnect_token_.empty()) {
    const auto now = domain::MonotonicClock::now();
    state_.connection_lost(now);
    registry_.mark_disconnected(reconnect_token_, generation_, state_.deadline());
    if (observer_store_) {
      observer_store_->upsert_session({observer_session_id_, player_id_, "Disconnected",
                                       observer_session_id_, generation_, now_epoch_milliseconds()});
      observer_store_->publish_event("session", "session.disconnected", observer_session_id_,
                                     observer_session_id_, player_id_ + " transport lost", "warning");
    }
    close_socket();
    stopped_ = true;
    if (!close_callback_sent_ && closed_callback_) {
      close_callback_sent_ = true;
      closed_callback_();
    }
    return;
  }
  final_stop();
}

void Session::final_stop() {
  if (stopped_) {
    return;
  }
  stopped_ = true;
  state_.close();
  if (observer_store_) {
    observer_store_->upsert_session({observer_session_id_, player_id_, "Closed",
                                     observer_session_id_, generation_, now_epoch_milliseconds()});
    observer_store_->publish_event("session", "session.closed", observer_session_id_,
                                   observer_session_id_, "session resources released");
  }
  deadline_timer_.cancel();
  close_socket();
  if (!reconnect_token_.empty()) {
    registry_.revoke(reconnect_token_, generation_);
  }
  if (!close_callback_sent_ && closed_callback_) {
    close_callback_sent_ = true;
    closed_callback_();
  }
}

void Session::close_socket() noexcept {
  boost::system::error_code ignored;
  socket_.shutdown(tcp::socket::shutdown_both, ignored);
  socket_.close(ignored);
}

std::uint64_t Session::now_epoch_milliseconds() const {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

}  // namespace arena_core::session
