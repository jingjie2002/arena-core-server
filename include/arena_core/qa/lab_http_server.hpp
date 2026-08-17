#pragma once

#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/core/error.hpp"
#include "arena_core/qa/lab_scenario.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <boost/asio.hpp>

namespace arena_core::qa {

class LabRunEngine;

struct LabHttpConfig {
  std::string bind_address{"127.0.0.1"};
  std::uint16_t port{8'090};
  std::filesystem::path static_root{"web/lab"};
  std::string write_header{"X-Arena-Lab"};
  std::size_t maximum_request_body_bytes{64U * 1024U};
};

// Local-only HTTP boundary for the scenario Lab. The server does not expose
// service credentials or arbitrary filesystem paths; all state-changing
// requests must pass the explicit write-header gate.
class LabHttpServer final : public std::enable_shared_from_this<LabHttpServer> {
 public:
  LabHttpServer(boost::asio::io_context& io_context,
                LabHttpConfig config,
                std::shared_ptr<LabRunEngine> engine,
                LabRunEnvironment environment,
                bool redis_fault_available);

  Error start();
  void stop();
  std::uint16_t bound_port() const;

 private:
  void do_accept();

  boost::asio::strand<boost::asio::io_context::executor_type> strand_;
  boost::asio::ip::tcp::acceptor acceptor_;
  LabHttpConfig config_;
  std::shared_ptr<LabRunEngine> engine_;
  LabRunEnvironment environment_;
  bool redis_fault_available_{false};
  bool started_{false};
  bool stopped_{false};
};

}  // namespace arena_core::qa
