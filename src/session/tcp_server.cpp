#include "arena_core/session/tcp_server.hpp"

#include "arena_core/observer/observer_store.hpp"

#include <chrono>
#include <utility>

#include <spdlog/spdlog.h>

namespace arena_core::session {
namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

}  // namespace

TcpServer::TcpServer(asio::io_context& io_context,
                     tcp::endpoint endpoint,
                     SessionOptions session_options,
                     std::shared_ptr<TokenGenerator> token_generator,
                     std::shared_ptr<BusinessService> business_service,
                     std::shared_ptr<observer::ObserverStore> observer_store)
    : strand_(asio::make_strand(io_context)),
      acceptor_(strand_),
      endpoint_(std::move(endpoint)),
      sweep_timer_(strand_),
      session_options_(std::move(session_options)),
      registry_(std::move(token_generator)),
      business_service_(std::move(business_service)),
      observer_store_(std::move(observer_store)) {}

Error TcpServer::start() {
  if (started_) {
    return {};
  }
  boost::system::error_code error;
  acceptor_.open(endpoint_.protocol(), error);
  if (error) {
    return make_error(ErrorCode::kInternalError, "TCP acceptor open failed");
  }
  acceptor_.set_option(tcp::acceptor::reuse_address(true), error);
  acceptor_.bind(endpoint_, error);
  if (error) {
    return make_error(ErrorCode::kInternalError, "TCP bind failed");
  }
  acceptor_.listen(asio::socket_base::max_listen_connections, error);
  if (error) {
    return make_error(ErrorCode::kInternalError, "TCP listen failed");
  }
  started_ = true;
  stopped_ = false;
  if (observer_store_) {
    observer_store_->set_health("tcp", true, "accepting");
  }
  if (business_service_) {
    std::weak_ptr<TcpServer> weak_self = shared_from_this();
    business_service_->set_player_notifier(
        [weak_self](const std::string_view player_id, protocol::v1::Envelope envelope) {
          if (const auto self = weak_self.lock()) {
            self->push_to_player(player_id, std::move(envelope));
          }
        });
    business_service_->start();
  }
  do_accept();
  arm_registry_sweep();
  return {};
}

void TcpServer::stop() {
  asio::dispatch(strand_, [self = shared_from_this()] {
    if (self->stopped_) {
      return;
    }
    self->stopped_ = true;
    if (self->observer_store_) {
      self->observer_store_->set_health("tcp", false, "draining");
    }
    boost::system::error_code ignored;
    self->acceptor_.cancel(ignored);
    self->acceptor_.close(ignored);
    self->sweep_timer_.cancel();
    if (self->business_service_) {
      self->business_service_->stop();
    }
    const auto sessions = self->sessions_;
    for (const auto& [id, session] : sessions) {
      static_cast<void>(id);
      session->request_stop();
    }
  });
}

std::uint16_t TcpServer::bound_port() const {
  boost::system::error_code error;
  const auto endpoint = acceptor_.local_endpoint(error);
  return error ? 0 : endpoint.port();
}

void TcpServer::do_accept() {
  acceptor_.async_accept(asio::bind_executor(strand_,
      [self = shared_from_this()](const boost::system::error_code& error, tcp::socket socket) {
        if (!error && !self->stopped_) {
          const auto session_id = ++self->next_session_id_;
          std::weak_ptr<TcpServer> weak_self = self;
          auto session = std::make_shared<Session>(
              std::move(socket), self->registry_, self->session_options_, self->business_service_,
              [weak_self, session_id] {
                if (const auto server = weak_self.lock()) {
                  asio::post(server->strand_, [server, session_id] { server->remove_session(session_id); });
                }
              }, self->observer_store_, "session-" + std::to_string(session_id));
          self->sessions_.emplace(session_id, session);
          self->active_connection_count_.fetch_add(1, std::memory_order_relaxed);
          session->start();
        }
        if (!self->stopped_) {
          self->do_accept();
        }
      }));
}

void TcpServer::push_to_player(const std::string_view player_id, protocol::v1::Envelope envelope) {
  if (const auto transport = registry_.find_transport(player_id)) {
    transport->push(std::move(envelope));
  }
}

void TcpServer::remove_session(const std::uint64_t session_id) {
  if (sessions_.erase(session_id) > 0U) {
    active_connection_count_.fetch_sub(1, std::memory_order_relaxed);
    if (observer_store_) {
      observer_store_->remove_session("session-" + std::to_string(session_id));
    }
  }
}

void TcpServer::arm_registry_sweep() {
  sweep_timer_.expires_after(std::chrono::seconds(1));
  sweep_timer_.async_wait(asio::bind_executor(strand_,
      [self = shared_from_this()](const boost::system::error_code& error) {
        if (error || self->stopped_) {
          return;
        }
        self->registry_.sweep_expired(domain::MonotonicClock::now());
        self->arm_registry_sweep();
      }));
}

}  // namespace arena_core::session
