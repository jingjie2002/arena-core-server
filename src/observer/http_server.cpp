#include "arena_core/observer/http_server.hpp"

#include "arena_core/observer/observer_store.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <map>
#include <string_view>
#include <utility>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

namespace arena_core::observer {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

struct ParsedTarget {
  std::string path;
  std::map<std::string, std::string> query;
};

ParsedTarget parse_target(const std::string_view target) {
  ParsedTarget result;
  const auto query_start = target.find('?');
  result.path = std::string(target.substr(0, query_start));
  if (query_start == std::string_view::npos) {
    return result;
  }
  auto remaining = target.substr(query_start + 1);
  while (!remaining.empty()) {
    const auto separator = remaining.find('&');
    const auto pair = remaining.substr(0, separator);
    const auto equals = pair.find('=');
    if (equals == std::string_view::npos) {
      result.query.emplace(std::string(pair), std::string{});
    } else {
      result.query.emplace(std::string(pair.substr(0, equals)), std::string(pair.substr(equals + 1)));
    }
    if (separator == std::string_view::npos) {
      break;
    }
    remaining.remove_prefix(separator + 1);
  }
  return result;
}

std::uint64_t parse_unsigned(const std::map<std::string, std::string>& query,
                             const std::string& key,
                             const std::uint64_t fallback) {
  const auto found = query.find(key);
  if (found == query.end() || found->second.empty()) {
    return fallback;
  }
  std::uint64_t value = 0;
  const auto* begin = found->second.data();
  const auto* end = begin + found->second.size();
  const auto parsed = std::from_chars(begin, end, value);
  return parsed.ec == std::errc{} && parsed.ptr == end ? value : fallback;
}

std::string content_type(const std::string_view path) {
  if (path.size() >= 3 && path.substr(path.size() - 3) == ".js") {
    return "text/javascript; charset=utf-8";
  }
  if (path.size() >= 4 && path.substr(path.size() - 4) == ".css") {
    return "text/css; charset=utf-8";
  }
  if (path.size() >= 4 && path.substr(path.size() - 4) == ".svg") {
    return "image/svg+xml";
  }
  return "text/html; charset=utf-8";
}

http::response<http::string_body> make_response(const http::status status,
                                                 const unsigned version,
                                                 std::string body,
                                                 std::string type = "application/json; charset=utf-8") {
  http::response<http::string_body> response{status, version};
  response.set(http::field::server, "arena-core-observer/1");
  response.set(http::field::content_type, std::move(type));
  response.set(http::field::cache_control, "no-store");
  response.set("X-Content-Type-Options", "nosniff");
  response.set("X-Frame-Options", "DENY");
  response.set("Referrer-Policy", "no-referrer");
  response.set("Content-Security-Policy",
               "default-src 'self'; connect-src 'self'; img-src 'self'; style-src 'self'; "
               "script-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'");
  response.keep_alive(false);
  response.body() = std::move(body);
  response.prepare_payload();
  return response;
}

http::response<http::string_body> json_response(const http::status status,
                                                 const unsigned version,
                                                 nlohmann::json body) {
  return make_response(status, version, body.dump());
}

http::response<http::string_body> static_response(const ObserverHttpConfig& config,
                                                   const unsigned version,
                                                   const std::string& path) {
  std::string filename;
  if (path == "/" || path == "/index.html") {
    filename = "index.html";
  } else if (path == "/app.js") {
    filename = "app.js";
  } else if (path == "/styles.css") {
    filename = "styles.css";
  } else if (path == "/favicon.svg") {
    filename = "favicon.svg";
  } else {
    return json_response(http::status::not_found, version, {{"error", "not_found"}});
  }
  std::ifstream input(config.static_root / filename, std::ios::binary);
  if (!input) {
    return json_response(http::status::not_found, version, {{"error", "static_asset_missing"}});
  }
  std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  return make_response(http::status::ok, version, std::move(body), content_type(filename));
}

http::response<http::string_body> route_request(const ObserverHttpConfig& config,
                                                 const std::shared_ptr<ObserverStore>& store,
                                                 const http::request<http::string_body>& request) {
  const auto target = parse_target(request.target());
  if (request.method() == http::verb::get && target.path == "/api/overview") {
    return json_response(http::status::ok, request.version(), store->overview_json());
  }
  if (request.method() == http::verb::get && target.path == "/api/sessions") {
    return json_response(http::status::ok, request.version(), store->sessions_json());
  }
  if (request.method() == http::verb::get && target.path == "/api/tickets") {
    return json_response(http::status::ok, request.version(), store->tickets_json());
  }
  if (request.method() == http::verb::get && target.path == "/api/rooms") {
    return json_response(http::status::ok, request.version(), store->rooms_json());
  }
  if (request.method() == http::verb::get && target.path == "/api/rankings") {
    return json_response(http::status::ok, request.version(), store->rankings_json());
  }
  if (request.method() == http::verb::get && target.path == "/api/qa/runs") {
    return json_response(http::status::ok, request.version(), store->qa_runs_json());
  }
  if (request.method() == http::verb::get && target.path == "/api/events") {
    const auto after = parse_unsigned(target.query, "after_seq", 0);
    const auto limit = static_cast<std::size_t>(parse_unsigned(target.query, "limit", 100));
    const auto trace = target.query.find("trace_id");
    return json_response(http::status::ok, request.version(),
                         store->events_json(after, limit, trace == target.query.end() ? "" : trace->second));
  }
  const std::string demo_prefix = "/api/demo/scenarios/";
  if (target.path.compare(0, demo_prefix.size(), demo_prefix) == 0) {
    if (request.method() != http::verb::post) {
      return json_response(http::status::method_not_allowed, request.version(),
                           {{"error", "method_not_allowed"}});
    }
    if (!config.demo_mode) {
      return json_response(http::status::forbidden, request.version(), {{"error", "demo_mode_disabled"}});
    }
    if (request["X-Arena-Demo"] != "1") {
      return json_response(http::status::forbidden, request.version(), {{"error", "demo_header_required"}});
    }
    const auto scenario = target.path.substr(demo_prefix.size());
    if (scenario.empty() || !store->run_demo_scenario(scenario)) {
      return json_response(http::status::not_found, request.version(), {{"error", "scenario_not_found"}});
    }
    return json_response(http::status::accepted, request.version(),
                         {{"scenario_id", scenario}, {"status", "completed"}});
  }
  if (target.path.compare(0, 5, "/api/") == 0) {
    return json_response(http::status::not_found, request.version(), {{"error", "not_found"}});
  }
  if (request.method() != http::verb::get) {
    return json_response(http::status::method_not_allowed, request.version(),
                         {{"error", "method_not_allowed"}});
  }
  return static_response(config, request.version(), target.path);
}

class HttpSession final : public std::enable_shared_from_this<HttpSession> {
 public:
  HttpSession(tcp::socket socket,
              ObserverHttpConfig config,
              std::shared_ptr<ObserverStore> store)
      : socket_(std::move(socket)), config_(std::move(config)), store_(std::move(store)) {
    parser_.body_limit(config_.maximum_request_body_bytes);
  }

  void start() {
    http::async_read(socket_, buffer_, parser_,
                     [self = shared_from_this()](const beast::error_code& error, std::size_t) {
                       if (error) {
                         self->close();
                         return;
                       }
                       self->write(route_request(self->config_, self->store_, self->parser_.get()));
                     });
  }

 private:
  void write(http::response<http::string_body> response) {
    response_ = std::make_shared<http::response<http::string_body>>(std::move(response));
    http::async_write(socket_, *response_,
                      [self = shared_from_this()](const beast::error_code&, std::size_t) { self->close(); });
  }

  void close() {
    beast::error_code ignored;
    socket_.shutdown(tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
  }

  tcp::socket socket_;
  beast::flat_buffer buffer_;
  http::request_parser<http::string_body> parser_;
  ObserverHttpConfig config_;
  std::shared_ptr<ObserverStore> store_;
  std::shared_ptr<http::response<http::string_body>> response_;
};

}  // namespace

ObserverHttpServer::ObserverHttpServer(asio::io_context& io_context,
                                       ObserverHttpConfig config,
                                       std::shared_ptr<ObserverStore> store)
    : strand_(asio::make_strand(io_context)),
      acceptor_(strand_),
      config_(std::move(config)),
      store_(std::move(store)) {}

Error ObserverHttpServer::start() {
  if (started_) {
    return {};
  }
  boost::system::error_code error;
  const auto address = asio::ip::make_address(config_.bind_address, error);
  if (error) {
    return make_error(ErrorCode::kConfigValidationFailed, "observer bind address is invalid");
  }
  const tcp::endpoint endpoint(address, config_.port);
  acceptor_.open(endpoint.protocol(), error);
  if (error) {
    return make_error(ErrorCode::kInternalError, "observer acceptor open failed");
  }
  acceptor_.set_option(tcp::acceptor::reuse_address(true), error);
  acceptor_.bind(endpoint, error);
  if (error) {
    return make_error(ErrorCode::kInternalError, "observer bind failed");
  }
  acceptor_.listen(asio::socket_base::max_listen_connections, error);
  if (error) {
    return make_error(ErrorCode::kInternalError, "observer listen failed");
  }
  started_ = true;
  stopped_ = false;
  store_->set_health("observer", true, config_.demo_mode ? "demo mode" : "read only");
  store_->publish_event("observer", "observer.started", "", "http", "observer API listening");
  do_accept();
  return {};
}

void ObserverHttpServer::stop() {
  asio::dispatch(strand_, [self = shared_from_this()] {
    if (self->stopped_) {
      return;
    }
    self->stopped_ = true;
    self->store_->set_health("observer", false, "stopped");
    boost::system::error_code ignored;
    self->acceptor_.cancel(ignored);
    self->acceptor_.close(ignored);
  });
}

std::uint16_t ObserverHttpServer::bound_port() const {
  boost::system::error_code error;
  const auto endpoint = acceptor_.local_endpoint(error);
  return error ? 0 : endpoint.port();
}

void ObserverHttpServer::do_accept() {
  acceptor_.async_accept(asio::bind_executor(
      strand_, [self = shared_from_this()](const boost::system::error_code& error, tcp::socket socket) {
        if (!error && !self->stopped_) {
          std::make_shared<HttpSession>(std::move(socket), self->config_, self->store_)->start();
        }
        if (!self->stopped_) {
          self->do_accept();
        }
      }));
}

}  // namespace arena_core::observer
