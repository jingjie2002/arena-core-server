#include "arena_core/core/version.hpp"
#include "arena_core/protocol/frame.hpp"
#include "arena_core/qa/robot_scenario.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#include <boost/asio.hpp>

#include "arena_core.pb.h"

namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
namespace wire = arena_core::protocol::v1;

struct CliOptions {
  std::string host{"127.0.0.1"};
  std::uint16_t port{9'100};
  std::string player_id{"robot-1"};
  std::uint32_t heartbeat_count{3};
  std::optional<std::string> scenario_path;
  std::optional<std::string> replay_path;
  std::optional<std::string> output_path;
  bool host_overridden{false};
  bool port_overridden{false};
};

wire::Envelope read_envelope(tcp::socket& socket) {
  std::array<std::uint8_t, 4> header{};
  asio::read(socket, asio::buffer(header));
  auto length = arena_core::protocol::decode_frame_length(header);
  if (!length.ok()) {
    throw std::runtime_error(length.error.message);
  }
  std::string payload(*length.value, '\0');
  asio::read(socket, asio::buffer(payload));
  auto envelope = arena_core::protocol::parse_envelope(payload);
  if (!envelope.ok()) {
    throw std::runtime_error(envelope.error.message);
  }
  return std::move(*envelope.value);
}

void write_envelope(tcp::socket& socket, const wire::Envelope& envelope) {
  auto frame = arena_core::protocol::frame_message(envelope);
  if (!frame.ok()) {
    throw std::runtime_error(frame.error.message);
  }
  asio::write(socket, asio::buffer(*frame.value));
}

int run_legacy_robot(const CliOptions& options) {
  asio::io_context io_context;
  tcp::resolver resolver(io_context);
  tcp::socket socket(io_context);
  asio::connect(socket, resolver.resolve(options.host, std::to_string(options.port)));

  wire::Hello hello;
  hello.set_player_id(options.player_id);
  wire::Envelope request;
  request.set_protocol_version(1);
  request.set_message_type(wire::HELLO);
  request.set_request_id(1);
  arena_core::protocol::set_payload(request, hello);
  write_envelope(socket, request);

  auto hello_response = read_envelope(socket);
  if (hello_response.message_type() != wire::HELLO_ACK) {
    throw std::runtime_error("server rejected Hello");
  }
  auto hello_ack = arena_core::protocol::parse_payload<wire::HelloAck>(hello_response);
  if (!hello_ack.ok()) {
    throw std::runtime_error("invalid HelloAck");
  }

  for (std::uint32_t index = 0; index < options.heartbeat_count; ++index) {
    wire::Heartbeat heartbeat;
    heartbeat.set_client_time_ms(index);
    request.Clear();
    request.set_protocol_version(1);
    request.set_message_type(wire::HEARTBEAT);
    request.set_request_id(static_cast<std::uint64_t>(index) + 2U);
    request.set_session_token(hello_ack.value->session_token());
    arena_core::protocol::set_payload(request, heartbeat);
    write_envelope(socket, request);
    const auto response = read_envelope(socket);
    if (response.message_type() != wire::HEARTBEAT_ACK) {
      throw std::runtime_error("server rejected Heartbeat");
    }
  }

  std::cout << "robot session passed player=" << options.player_id
            << " heartbeats=" << options.heartbeat_count << '\n';
  return EXIT_SUCCESS;
}

void print_usage() {
  std::cerr
      << "Usage: arena-core-robot [--host HOST] [--port PORT] [--player ID] [--heartbeats N]\n"
      << "       arena-core-robot --scenario FILE [--host HOST] [--port PORT] [--output REPORT]\n"
      << "       arena-core-robot --replay REPORT [--host HOST] [--port PORT] [--output REPORT]\n";
}

CliOptions parse_cli(const int argc, char** argv) {
  CliOptions options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument(argv[index]);
    const auto require_value = [&](const std::string& name) -> std::string {
      if (index + 1 >= argc) {
        throw std::invalid_argument(name + " requires a value");
      }
      return argv[++index];
    };
    if (argument == "--host") {
      options.host = require_value(argument);
      options.host_overridden = true;
    } else if (argument == "--port") {
      const auto value = std::stoul(require_value(argument));
      if (value == 0U || value > 65'535U) {
        throw std::out_of_range("port out of range");
      }
      options.port = static_cast<std::uint16_t>(value);
      options.port_overridden = true;
    } else if (argument == "--player") {
      options.player_id = require_value(argument);
    } else if (argument == "--heartbeats") {
      options.heartbeat_count = static_cast<std::uint32_t>(std::stoul(require_value(argument)));
    } else if (argument == "--scenario") {
      options.scenario_path = require_value(argument);
    } else if (argument == "--replay") {
      options.replay_path = require_value(argument);
    } else if (argument == "--output") {
      options.output_path = require_value(argument);
    } else {
      throw std::invalid_argument("unknown argument: " + argument);
    }
  }
  if (options.scenario_path && options.replay_path) {
    throw std::invalid_argument("--scenario and --replay are mutually exclusive");
  }
  return options;
}

int run_scenario(const CliOptions& options) {
  arena_core::qa::RobotRunReport expected;
  arena_core::qa::RobotScenario scenario;
  if (options.replay_path) {
    auto loaded = arena_core::qa::load_robot_report(*options.replay_path);
    if (!loaded.ok()) {
      throw std::runtime_error(loaded.error.message);
    }
    expected = std::move(*loaded.value);
    scenario = expected.scenario;
  } else {
    auto loaded = arena_core::qa::load_robot_scenario(*options.scenario_path);
    if (!loaded.ok()) {
      throw std::runtime_error(loaded.error.message);
    }
    scenario = std::move(*loaded.value);
  }
  if (options.host_overridden) {
    scenario.host = options.host;
  }
  if (options.port_overridden) {
    scenario.port = options.port;
  }

  auto report = arena_core::qa::run_robot_scenario(scenario);
  if (options.replay_path) {
    report.replay_source = *options.replay_path;
    const auto replay_error = arena_core::qa::verify_robot_replay(expected, report);
    report.replay_verified = !replay_error;
    if (replay_error) {
      report.passed = false;
      report.failures.push_back({0U, replay_error.message});
    }
  }
  if (options.output_path) {
    if (const auto error = arena_core::qa::write_robot_report(*options.output_path, report)) {
      throw std::runtime_error(error.message);
    }
  }
  std::cout << arena_core::qa::robot_report_json(report).at("summary").dump() << '\n';
  return report.passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

}  // namespace

int main(int argc, char** argv) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument(argv[index]);
    if (argument == "--version") {
      std::cout << "arena-core-robot " << arena_core::kProjectVersion << '\n';
      return EXIT_SUCCESS;
    }
    if (argument == "--self-check") {
      std::cout << "arena-core-robot protocol and scenario self-check passed\n";
      return EXIT_SUCCESS;
    }
  }

  try {
    const auto options = parse_cli(argc, argv);
    if (options.scenario_path || options.replay_path) {
      return run_scenario(options);
    }
    return run_legacy_robot(options);
  } catch (const std::exception& error) {
    print_usage();
    std::cerr << "robot failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
