#include "arena_core/qa/lab_http_server.hpp"

#include "arena_core/qa/lab_run.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string_view>
#include <utility>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

namespace arena_core::qa {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

constexpr std::size_t kMaximumBodyBytes = 64U * 1024U;

struct ParsedTarget {
  std::string path;
  std::map<std::string, std::string> query;
};

ParsedTarget parse_target(const beast::string_view target) {
  const std::string_view view(target.data(), target.size());
  ParsedTarget result;
  const auto query_start = view.find('?');
  result.path = std::string(view.substr(0, query_start));
  if (query_start == std::string_view::npos) return result;

  auto remaining = view.substr(query_start + 1U);
  while (!remaining.empty()) {
    const auto separator = remaining.find('&');
    const auto pair = remaining.substr(0, separator);
    const auto equals = pair.find('=');
    if (equals == std::string_view::npos) {
      result.query.emplace(std::string(pair), std::string{});
    } else {
      result.query.emplace(std::string(pair.substr(0, equals)),
                           std::string(pair.substr(equals + 1U)));
    }
    if (separator == std::string_view::npos) break;
    remaining.remove_prefix(separator + 1U);
  }
  return result;
}

std::optional<std::uint64_t> parse_unsigned(const std::string& value) {
  if (value.empty()) return std::nullopt;
  std::uint64_t result = 0;
  const auto* begin = value.data();
  const auto* end = begin + value.size();
  const auto parsed = std::from_chars(begin, end, result);
  if (parsed.ec != std::errc{} || parsed.ptr != end) return std::nullopt;
  return result;
}

std::string trim_ascii(std::string value) {
  const auto not_space = [](const unsigned char character) {
    return std::isspace(character) == 0;
  };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
  return value;
}

bool has_json_content_type(const http::request<http::string_body>& request) {
  const auto found = request.find(http::field::content_type);
  if (found == request.end()) return false;
  std::string value(found->value().data(), found->value().size());
  const auto semicolon = value.find(';');
  value = trim_ascii(value.substr(0, semicolon));
  std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value == "application/json";
}

bool has_write_header(const LabHttpConfig& config,
                      const http::request<http::string_body>& request) {
  std::size_t matches = 0;
  bool value_matches = false;
  for (const auto& field : request.base()) {
    if (!beast::iequals(field.name_string(), config.write_header)) continue;
    ++matches;
    value_matches = field.value() == "1";
  }
  return matches == 1U && value_matches;
}

std::string content_type(const std::string_view filename) {
  if (filename.size() >= 3U && filename.substr(filename.size() - 3U) == ".js") {
    return "text/javascript; charset=utf-8";
  }
  if (filename.size() >= 4U && filename.substr(filename.size() - 4U) == ".css") {
    return "text/css; charset=utf-8";
  }
  if (filename.size() >= 4U && filename.substr(filename.size() - 4U) == ".svg") {
    return "image/svg+xml";
  }
  return "text/html; charset=utf-8";
}

http::response<http::string_body> make_response(const LabHttpConfig& config,
                                                 const http::status status,
                                                 const unsigned version,
                                                 std::string body,
                                                 std::string type =
                                                     "application/json; charset=utf-8") {
  http::response<http::string_body> response{status, version};
  response.set(http::field::server, "arena-core-lab/1");
  response.set(http::field::content_type, std::move(type));
  response.set(http::field::cache_control, "no-store");
  response.set(http::field::pragma, "no-cache");
  response.set(config.write_header, "1");
  response.set("X-Content-Type-Options", "nosniff");
  response.set("X-Frame-Options", "DENY");
  response.set("Referrer-Policy", "no-referrer");
  response.set("Cross-Origin-Resource-Policy", "same-origin");
  response.set("Permissions-Policy", "camera=(), microphone=(), geolocation=()");
  response.set("Content-Security-Policy",
               "default-src 'self'; connect-src 'self'; img-src 'self'; style-src 'self'; "
               "script-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'; "
               "form-action 'self'");
  response.keep_alive(false);
  response.body() = std::move(body);
  response.prepare_payload();
  return response;
}

http::response<http::string_body> json_response(const LabHttpConfig& config,
                                                 const http::status status,
                                                 const unsigned version,
                                                 nlohmann::json body) {
  return make_response(config, status, version, body.dump());
}

http::response<http::string_body> json_error(const LabHttpConfig& config,
                                              const http::status status,
                                              const unsigned version,
                                              std::string code,
                                              std::string message) {
  if (message.empty()) message = code;
  return json_response(config, status, version,
                       {{"error", code}, {"code", std::move(code)},
                        {"message", std::move(message)}});
}

http::status status_for_error(const ErrorCode code) {
  switch (code) {
    case ErrorCode::kQaScenarioInvalid:
    case ErrorCode::kConfigParseFailed:
    case ErrorCode::kConfigValidationFailed:
      return http::status::bad_request;
    case ErrorCode::kQaRunNotFound:
      return http::status::not_found;
    case ErrorCode::kQaRunAlreadyActive:
    case ErrorCode::kQaRunNotCancellable:
    case ErrorCode::kQaRunCancelled:
    case ErrorCode::kQaRunTimedOut:
      return http::status::conflict;
    case ErrorCode::kDependencyRedisUnavailable:
    case ErrorCode::kDependencyMysqlUnavailable:
    case ErrorCode::kServerBusy:
    case ErrorCode::kServerShuttingDown:
      return http::status::service_unavailable;
    default:
      return http::status::internal_server_error;
  }
}

http::response<http::string_body> error_response(const LabHttpConfig& config,
                                                  const unsigned version,
                                                  const Error& error) {
  const auto code = std::string(error_code_name(error.code));
  return json_error(config, status_for_error(error.code), version, code, error.message);
}

http::response<http::string_body> static_response(const LabHttpConfig& config,
                                                   const unsigned version,
                                                   const std::string& path) {
  static const std::map<std::string, std::string> allowed{
      {"/", "overview.html"},
      {"/overview.html", "overview.html"},
      {"/scenarios.html", "scenarios.html"},
      {"/runs.html", "runs.html"},
      {"/integration.html", "integration.html"},
      {"/styles.css", "styles.css"},
      {"/app.js", "app.js"},
      {"/favicon.svg", "favicon.svg"},
  };
  const auto found = allowed.find(path);
  if (found == allowed.end()) {
    return json_error(config, http::status::not_found, version, "NOT_FOUND",
                      "static asset is not available");
  }

  std::ifstream input(config.static_root / found->second, std::ios::binary);
  if (!input) {
    return json_error(config, http::status::not_found, version, "STATIC_ASSET_MISSING",
                      "static asset is missing");
  }
  std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  return make_response(config, http::status::ok, version, std::move(body),
                       content_type(found->second));
}

http::response<http::string_body> method_not_allowed(const LabHttpConfig& config,
                                                      const unsigned version) {
  return json_error(config, http::status::method_not_allowed, version,
                    "METHOD_NOT_ALLOWED", "HTTP method is not allowed for this route");
}

http::response<http::string_body> route_scenario_schema(
    const LabHttpConfig& config,
    const bool redis_fault_available,
    const http::request<http::string_body>& request,
    const std::string& path) {
  const std::string prefix = path.compare(0, 22, "/api/lab/v1/scenarios/") == 0
                                 ? "/api/lab/v1/scenarios/"
                                 : "/api/lab/scenarios/";
  constexpr std::string_view suffix{"/schema"};
  if (path.size() <= prefix.size() + suffix.size() ||
      path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0) {
    return json_error(config, http::status::not_found, request.version(), "NOT_FOUND",
                      "scenario schema was not found");
  }
  const auto name = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
  if (name.empty() || name.find('/') != std::string::npos) {
    return json_error(config, http::status::not_found, request.version(), "NOT_FOUND",
                      "scenario schema was not found");
  }
  if (request.method() != http::verb::get) return method_not_allowed(config, request.version());
  auto kind = parse_lab_scenario_kind(name);
  if (!kind.ok()) {
    return json_error(config, http::status::not_found, request.version(), "QA_SCENARIO_NOT_FOUND",
                      "scenario schema was not found");
  }
  auto schema = lab_scenario_schema(*kind.value);
  schema["available"] = name != "redis_failure_recovery" || redis_fault_available;
  return json_response(config, http::status::ok, request.version(), std::move(schema));
}

http::response<http::string_body> route_run_resource(
  const LabHttpConfig& config,
  const std::shared_ptr<LabRunEngine>& engine,
    const http::request<http::string_body>& request,
    const ParsedTarget& target) {
  const std::string_view prefix =
      target.path.compare(0, 17, "/api/lab/v1/runs/") == 0
          ? std::string_view{"/api/lab/v1/runs/"}
          : std::string_view{"/api/lab/runs/"};
  const auto rest = target.path.substr(prefix.size());
  const auto separator = rest.find('/');
  const auto run_id = rest.substr(0, separator);
  const auto action = separator == std::string::npos ? std::string{} : rest.substr(separator + 1U);
  if (run_id.empty()) {
    return json_error(config, http::status::not_found, request.version(), "QA_RUN_NOT_FOUND",
                      "Run was not found");
  }

  if (action.empty()) {
    if (request.method() != http::verb::get) return method_not_allowed(config, request.version());
    auto run = engine->get(run_id);
    if (!run.ok()) return error_response(config, request.version(), run.error);
    return json_response(config, http::status::ok, request.version(),
                         lab_run_report_json(*run.value));
  }

  if (action == "events") {
    if (request.method() != http::verb::get) return method_not_allowed(config, request.version());
    std::uint64_t after_seq = 0;
    const auto after = target.query.find("after_seq");
    if (after != target.query.end()) {
      const auto parsed = parse_unsigned(after->second);
      if (!parsed.has_value()) {
        return json_error(config, http::status::bad_request, request.version(),
                          "QA_SCENARIO_INVALID", "after_seq must be an unsigned integer");
      }
      after_seq = *parsed;
    }
    auto run = engine->get(run_id);
    if (!run.ok()) return error_response(config, request.version(), run.error);
    return json_response(config, http::status::ok, request.version(),
                         engine->events(run_id, after_seq));
  }

  if (action == "replay") {
    if (request.method() != http::verb::post) return method_not_allowed(config, request.version());
    auto replay = engine->replay(run_id);
    if (!replay.ok()) return error_response(config, request.version(), replay.error);
    return json_response(config, http::status::accepted, request.version(),
                         lab_run_report_json(*replay.value));
  }

  if (action == "cancel") {
    if (request.method() != http::verb::post) return method_not_allowed(config, request.version());
    auto existing = engine->get(run_id);
    if (!existing.ok()) return error_response(config, request.version(), existing.error);
    if (const auto error = engine->cancel(run_id)) {
      return error_response(config, request.version(), error);
    }
    auto current = engine->get(run_id);
    return json_response(config, http::status::accepted, request.version(),
                         current.ok() ? lab_run_report_json(*current.value)
                                      : nlohmann::json{{"run_id", run_id},
                                                       {"status", "Cancelling"}});
  }

  return json_error(config, http::status::not_found, request.version(), "NOT_FOUND",
                    "Run route was not found");
}

http::response<http::string_body> route_request(
    const LabHttpConfig& config,
    const LabRunEnvironment& environment,
    const bool redis_fault_available,
    const std::shared_ptr<LabRunEngine>& engine,
    const http::request<http::string_body>& request) {
  if (request.method() == http::verb::post) {
    if (!has_write_header(config, request)) {
      return json_error(config, http::status::forbidden, request.version(),
                        "LAB_WRITE_HEADER_REQUIRED",
                        config.write_header + " must be exactly 1");
    }
    if (!has_json_content_type(request)) {
      return json_error(config, http::status::unsupported_media_type, request.version(),
                        "CONTENT_TYPE_UNSUPPORTED", "Content-Type must be application/json");
    }
  }

  const auto target = parse_target(request.target());
  if (target.path == "/api/lab/v1/health" || target.path == "/api/lab/health") {
    if (request.method() != http::verb::get) return method_not_allowed(config, request.version());
    return json_response(config, http::status::ok, request.version(),
                         {{"status", "ok"}, {"active_run", engine->active()},
                          {"schema_version", 1}});
  }

  if (target.path == "/api/lab/v1/capabilities" ||
      target.path == "/api/lab/capabilities") {
    if (request.method() != http::verb::get) return method_not_allowed(config, request.version());
    auto capabilities = lab_capabilities(environment, redis_fault_available);
    for (auto& scenario : capabilities["scenarios"]) {
      scenario["schema_url"] = "/api/lab/v1/scenarios/" +
                               scenario.at("name").get<std::string>() + "/schema";
    }
    return json_response(config, http::status::ok, request.version(), std::move(capabilities));
  }

  if (target.path.compare(0, 19, "/api/lab/scenarios/") == 0 ||
      target.path.compare(0, 22, "/api/lab/v1/scenarios/") == 0) {
    return route_scenario_schema(config, redis_fault_available, request, target.path);
  }

  if (target.path == "/api/lab/v1/runs" || target.path == "/api/lab/runs") {
    if (request.method() != http::verb::post) return method_not_allowed(config, request.version());
    nlohmann::json value;
    try {
      value = nlohmann::json::parse(request.body());
    } catch (const nlohmann::json::exception& error) {
      return json_error(config, http::status::bad_request, request.version(),
                        "QA_SCENARIO_INVALID", std::string("invalid JSON: ") + error.what());
    }
    auto parsed = parse_lab_run_request(value);
    if (!parsed.ok()) return error_response(config, request.version(), parsed.error);
    if (parsed.value->kind == LabScenarioKind::kRedisFailureRecovery &&
        !redis_fault_available) {
      return json_error(config, http::status::service_unavailable, request.version(),
                        "QA_SCENARIO_UNAVAILABLE",
                        "Redis fault injection is unavailable in this Lab profile");
    }
    auto created = engine->create(std::move(*parsed.value));
    if (!created.ok()) return error_response(config, request.version(), created.error);
    return json_response(config, http::status::accepted, request.version(),
                         lab_run_report_json(*created.value));
  }

  if (target.path.compare(0, 17, "/api/lab/v1/runs/") == 0 ||
      target.path.compare(0, 14, "/api/lab/runs/") == 0) {
    return route_run_resource(config, engine, request, target);
  }

  if (target.path.compare(0, 9, "/api/lab/") == 0) {
    return json_error(config, http::status::not_found, request.version(), "NOT_FOUND",
                      "Lab API route was not found");
  }
  if (request.method() != http::verb::get) return method_not_allowed(config, request.version());
  return static_response(config, request.version(), target.path);
}

class HttpSession final : public std::enable_shared_from_this<HttpSession> {
 public:
  HttpSession(tcp::socket socket,
              LabHttpConfig config,
              LabRunEnvironment environment,
              const bool redis_fault_available,
              std::shared_ptr<LabRunEngine> engine)
      : socket_(std::move(socket)),
        config_(std::move(config)),
        environment_(std::move(environment)),
        redis_fault_available_(redis_fault_available),
        engine_(std::move(engine)) {
    parser_.body_limit(config_.maximum_request_body_bytes);
  }

  void start() {
    http::async_read(socket_, buffer_, parser_,
                     [self = shared_from_this()](const beast::error_code& error, std::size_t) {
                       if (error == http::error::body_limit) {
                         self->write(json_error(self->config_, http::status::payload_too_large, 11,
                                                "REQUEST_BODY_TOO_LARGE",
                                                "request body exceeds 64 KiB"));
                         return;
                       }
                       if (error) {
                         self->close();
                         return;
                       }
                       self->write(route_request(self->config_, self->environment_,
                                                 self->redis_fault_available_, self->engine_,
                                                 self->parser_.get()));
                     });
  }

 private:
  void write(http::response<http::string_body> response) {
    response_ = std::make_shared<http::response<http::string_body>>(std::move(response));
    http::async_write(socket_, *response_,
                      [self = shared_from_this()](const beast::error_code&, std::size_t) {
                        self->close();
                      });
  }

  void close() {
    beast::error_code ignored;
    socket_.shutdown(tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
  }

  tcp::socket socket_;
  beast::flat_buffer buffer_;
  http::request_parser<http::string_body> parser_;
  LabHttpConfig config_;
  LabRunEnvironment environment_;
  bool redis_fault_available_{false};
  std::shared_ptr<LabRunEngine> engine_;
  std::shared_ptr<http::response<http::string_body>> response_;
};

}  // namespace

LabHttpServer::LabHttpServer(asio::io_context& io_context,
                             LabHttpConfig config,
                             std::shared_ptr<LabRunEngine> engine,
                             LabRunEnvironment environment,
                             const bool redis_fault_available)
    : strand_(asio::make_strand(io_context)),
      acceptor_(strand_),
      config_(std::move(config)),
      engine_(std::move(engine)),
      environment_(std::move(environment)),
      redis_fault_available_(redis_fault_available) {}

Error LabHttpServer::start() {
  if (started_ && !stopped_) return {};
  if (!engine_) {
    return make_error(ErrorCode::kConfigValidationFailed, "Lab Run engine is required");
  }
  if (config_.static_root.empty() || config_.write_header.empty()) {
    return make_error(ErrorCode::kConfigValidationFailed,
                      "Lab static root and write header are required");
  }
  const auto valid_header = std::all_of(
      config_.write_header.begin(), config_.write_header.end(), [](const unsigned char character) {
        return std::isalnum(character) != 0 || character == '-';
      });
  if (!valid_header) {
    return make_error(ErrorCode::kConfigValidationFailed,
                      "Lab write header must be an HTTP token");
  }
  if (config_.maximum_request_body_bytes == 0U ||
      config_.maximum_request_body_bytes > kMaximumBodyBytes) {
    return make_error(ErrorCode::kConfigValidationFailed,
                      "Lab request body limit must be between 1 and 64 KiB");
  }

  boost::system::error_code error;
  const auto address = asio::ip::make_address(config_.bind_address, error);
  if (error) {
    return make_error(ErrorCode::kConfigValidationFailed, "Lab bind address is invalid");
  }
  const tcp::endpoint endpoint(address, config_.port);
  acceptor_.open(endpoint.protocol(), error);
  if (error) return make_error(ErrorCode::kInternalError, "Lab acceptor open failed");
  acceptor_.set_option(tcp::acceptor::reuse_address(true), error);
  if (error) return make_error(ErrorCode::kInternalError, "Lab reuse-address setup failed");
  acceptor_.bind(endpoint, error);
  if (error) return make_error(ErrorCode::kInternalError, "Lab bind failed");
  acceptor_.listen(asio::socket_base::max_listen_connections, error);
  if (error) return make_error(ErrorCode::kInternalError, "Lab listen failed");

  started_ = true;
  stopped_ = false;
  do_accept();
  return {};
}

void LabHttpServer::stop() {
  asio::dispatch(strand_, [self = shared_from_this()] {
    if (self->stopped_) return;
    self->stopped_ = true;
    boost::system::error_code ignored;
    self->acceptor_.cancel(ignored);
    self->acceptor_.close(ignored);
  });
}

std::uint16_t LabHttpServer::bound_port() const {
  boost::system::error_code error;
  const auto endpoint = acceptor_.local_endpoint(error);
  return error ? 0 : endpoint.port();
}

void LabHttpServer::do_accept() {
  acceptor_.async_accept(asio::bind_executor(
      strand_, [self = shared_from_this()](const boost::system::error_code& error,
                                           tcp::socket socket) {
        if (!error && !self->stopped_) {
          std::make_shared<HttpSession>(std::move(socket), self->config_, self->environment_,
                                        self->redis_fault_available_, self->engine_)
              ->start();
        }
        if (!self->stopped_) self->do_accept();
      }));
}

}  // namespace arena_core::qa
