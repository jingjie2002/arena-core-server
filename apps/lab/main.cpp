#include "arena_core/core/error.hpp"
#include "arena_core/core/version.hpp"
#include "arena_core/qa/lab_app_config.hpp"
#include "arena_core/qa/lab_http_server.hpp"
#include "arena_core/qa/lab_run.hpp"
#include "arena_core/qa/lab_scenario.hpp"
#include "arena_core/qa/lab_scenarios.hpp"
#include "arena_core/qa/toxiproxy_client.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

namespace {

struct Options {
  std::filesystem::path config{"config/arena-core-lab.example.json"};
  bool check_config{false};
  bool self_check{false};
  bool health_check{false};
  bool reset_faults{false};
};

arena_core::Result<Options> parse_options(const int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument(argv[index]);
    if (argument == "--check-config") options.check_config = true;
    else if (argument == "--self-check") options.self_check = true;
    else if (argument == "--health-check") options.health_check = true;
    else if (argument == "--reset-faults") options.reset_faults = true;
    else if (argument == "--config" && index + 1 < argc) options.config = argv[++index];
    else {
      return arena_core::Result<Options>::failure(arena_core::make_error(
          arena_core::ErrorCode::kConfigValidationFailed,
          "usage: arena-core-lab [--config PATH] "
          "[--check-config|--self-check|--health-check|--reset-faults]"));
    }
  }
  return arena_core::Result<Options>::success(std::move(options));
}

int fail(const arena_core::Error& error) {
  std::cerr << arena_core::error_code_name(error.code) << ": " << error.message << '\n';
  return 1;
}

bool healthy_local_listener(const arena_core::qa::LabHttpAppConfig& config) {
  namespace asio = boost::asio;
  namespace beast = boost::beast;
  namespace http = beast::http;
  using tcp = asio::ip::tcp;
  try {
    asio::io_context io_context;
    tcp::resolver resolver(io_context);
    beast::tcp_stream stream(io_context);
    stream.expires_after(std::chrono::seconds(2));
    const auto host = config.bind_address == "0.0.0.0" ? std::string("127.0.0.1")
        : config.bind_address == "::" ? std::string("::1") : config.bind_address;
    stream.connect(resolver.resolve(host, std::to_string(config.port)));
    http::request<http::empty_body> request{http::verb::get, "/api/lab/v1/health", 11};
    request.set(http::field::host, host);
    request.set(http::field::user_agent, "arena-core-lab-health/1");
    http::write(stream, request);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(stream, buffer, response);
    const auto body = nlohmann::json::parse(response.body(), nullptr, false);
    return response.result() == http::status::ok && body.is_object() &&
           body.value("status", std::string{}) == "ok";
  } catch (const std::exception&) {
    return false;
  }
}

}  // namespace

int main(const int argc, char** argv) {
  const auto options = parse_options(argc, argv);
  if (!options.ok()) return fail(options.error);
  if (options.value->health_check) {
    const auto config = arena_core::qa::load_lab_app_config(options.value->config);
    if (!config.ok()) return fail(config.error);
    return healthy_local_listener(config.value->http) ? 0 : 1;
  }
  if (options.value->self_check) {
    const auto schema = arena_core::qa::lab_scenario_schema(
        arena_core::qa::LabScenarioKind::kCompleteMatch);
    if (schema.value("schema_version", 0) != 1) return 1;
    std::cout << "arena-core-lab " << arena_core::kProjectVersion << " self-check passed\n";
    return 0;
  }
  const auto config = arena_core::qa::load_lab_app_config(options.value->config);
  if (!config.ok()) return fail(config.error);
  if (options.value->check_config) {
    std::cout << "Lab configuration is valid\n";
    return 0;
  }
  if (options.value->reset_faults) {
    if (!config.value->toxiproxy.enabled) {
      return fail(arena_core::make_error(arena_core::ErrorCode::kConfigValidationFailed,
                                         "Toxiproxy is disabled in this Lab configuration"));
    }
    arena_core::qa::ToxiproxyClientConfig proxy;
    proxy.host = config.value->toxiproxy.host;
    proxy.api_port = config.value->toxiproxy.port;
    proxy.proxy_name = config.value->toxiproxy.proxy_name;
    proxy.listen = config.value->toxiproxy.listen;
    proxy.upstream = config.value->toxiproxy.upstream;
    const auto error = arena_core::qa::ToxiproxyClient(std::move(proxy)).reset();
    if (error) return fail(error);
    std::cout << "Lab dependency faults reset\n";
    return 0;
  }
  const auto environment = arena_core::qa::lab_run_environment(*config.value);
  if (!environment.ok()) return fail(environment.error);

  try {
    boost::asio::io_context io_context;
    auto engine = std::make_shared<arena_core::qa::LabRunEngine>(
        *environment.value, config.value->reports.directory,
        arena_core::qa::make_lab_scenario_handlers());
    if (const auto error = engine->start()) return fail(error);

    arena_core::qa::LabHttpConfig http_config;
    http_config.bind_address = config.value->http.bind_address;
    http_config.port = config.value->http.port;
    http_config.static_root = config.value->http.static_root;
    http_config.maximum_request_body_bytes = config.value->http.maximum_request_body_bytes;
    auto server = std::make_shared<arena_core::qa::LabHttpServer>(
        io_context, std::move(http_config), engine, *environment.value,
        config.value->toxiproxy.enabled);
    if (const auto error = server->start()) return fail(error);

    boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code&, int) {
      server->stop();
      engine->stop();
      io_context.stop();
    });
    const auto count = std::max(2U, std::thread::hardware_concurrency() == 0U
        ? 2U : std::min(4U, std::thread::hardware_concurrency()));
    std::vector<std::thread> workers;
    workers.reserve(count - 1U);
    for (unsigned index = 1; index < count; ++index) {
      workers.emplace_back([&] { io_context.run(); });
    }
    std::cout << "arena-core-lab listening on " << config.value->http.bind_address << ':'
              << server->bound_port() << '\n';
    io_context.run();
    for (auto& worker : workers) worker.join();
    engine->stop();
    return 0;
  } catch (const std::exception& error) {
    return fail(arena_core::make_error(arena_core::ErrorCode::kInternalError,
                                       error.what()));
  }
}
