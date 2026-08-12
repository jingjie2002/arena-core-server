#include "arena_core/core/config.hpp"
#include "arena_core/core/logging.hpp"
#include "arena_core/core/version.hpp"
#include "arena_core/domain/identifiers.hpp"
#include "arena_core/game/game_service.hpp"
#include "arena_core/observer/http_server.hpp"
#include "arena_core/observer/observer_store.hpp"
#include "arena_core/rank/rank_cache.hpp"
#include "arena_core/rules/rule_engine.hpp"
#include "arena_core/session/tcp_server.hpp"
#include "arena_core/session/token_generator.hpp"
#include "arena_core/settlement/persistent_settlement_service.hpp"
#include "arena_core/storage/settlement_repository.hpp"
#include "arena_core/storage/storage_executor.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <spdlog/spdlog.h>

namespace {

void print_usage() {
  std::cout << "Usage: arena-core-server [--config FILE] [--demo-mode] "
               "[--check-config|--self-check|--version]\n";
}

std::string environment_value(const std::string& name) {
  const auto* value = std::getenv(name.c_str());
  return value == nullptr ? std::string{} : std::string(value);
}

std::uint64_t runtime_id_seed() {
  const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  const auto entropy = static_cast<std::uint64_t>(std::random_device{}());
  return (now << 20U) ^ entropy;
}

}  // namespace

int main(int argc, char** argv) {
  std::optional<std::filesystem::path> config_path;
  bool self_check = false;
  bool check_config = false;
  bool demo_mode = false;

  for (int index = 1; index < argc; ++index) {
    const std::string argument(argv[index]);
    if (argument == "--version") {
      std::cout << arena_core::kProjectName << ' ' << arena_core::kProjectVersion << " protocol="
                << arena_core::kProtocolVersion << '\n';
      return EXIT_SUCCESS;
    }
    if (argument == "--help") {
      print_usage();
      return EXIT_SUCCESS;
    }
    if (argument == "--self-check") {
      self_check = true;
      continue;
    }
    if (argument == "--check-config") {
      check_config = true;
      continue;
    }
    if (argument == "--demo-mode") {
      demo_mode = true;
      continue;
    }
    if (argument == "--config" && index + 1 < argc) {
      config_path = std::filesystem::path(argv[++index]);
      continue;
    }
    std::cerr << "unknown or incomplete argument: " << argument << '\n';
    print_usage();
    return EXIT_FAILURE;
  }

  arena_core::AppConfig config;
  if (config_path.has_value()) {
    auto result = arena_core::load_app_config(*config_path);
    if (!result.ok()) {
      std::cerr << arena_core::error_code_name(result.error.code) << ": " << result.error.message << '\n';
      return EXIT_FAILURE;
    }
    config = std::move(*result.value);
  } else if (check_config) {
    std::cerr << "--check-config requires --config FILE\n";
    return EXIT_FAILURE;
  }
  if (demo_mode) {
    config.observer.demo_mode = true;
    if (const auto error = arena_core::validate_app_config(config)) {
      std::cerr << arena_core::error_code_name(error.code) << ": " << error.message << '\n';
      return EXIT_FAILURE;
    }
  }

  if (const auto error = arena_core::initialize_logging(config.logging, "arena-core-server")) {
    std::cerr << arena_core::error_code_name(error.code) << ": " << error.message << '\n';
    return EXIT_FAILURE;
  }

  if (check_config) {
    spdlog::info("configuration valid environment={} tcp_port={} observer_port={}", config.environment,
                 config.network.tcp_port, config.network.observer_port);
    return EXIT_SUCCESS;
  }
  if (self_check) {
    spdlog::info("foundation and protocol self-check passed cpp_standard=17 protocol=1");
    return EXIT_SUCCESS;
  }

  try {
    boost::asio::io_context io_context;
    arena_core::session::SessionOptions options;
    options.maximum_frame_bytes = config.network.max_frame_bytes;
    options.durations.hello_deadline = std::chrono::milliseconds(config.session.hello_timeout_ms);
    options.durations.idle_timeout = std::chrono::milliseconds(config.session.idle_timeout_ms);
    options.durations.reconnect_window = std::chrono::milliseconds(config.session.reconnect_window_ms);

    const auto address = boost::asio::ip::make_address(config.network.bind_address);
    auto token_generator = std::make_shared<arena_core::session::RandomTokenGenerator>();
    std::shared_ptr<arena_core::observer::ObserverStore> observer_store;
    if (config.observer.enabled) {
      observer_store = std::make_shared<arena_core::observer::ObserverStore>(config.observer.max_events);
      observer_store->set_health("mysql", true, config.data.enabled ? "checking" : "not configured");
      observer_store->set_health("redis", true,
                                 config.data.enabled ? "configured; lazy health check" : "not configured");
    }
    std::shared_ptr<arena_core::storage::StorageExecutor> storage_executor;
    std::shared_ptr<arena_core::settlement::SettlementGateway> settlement_gateway;
    if (config.data.enabled) {
      auto rules = arena_core::rules::LuaRuleEngine::load(config.data.rules.match_script,
                                                          config.data.rules.score_script);
      if (!rules.ok()) {
        spdlog::error("{}: {}", arena_core::error_code_name(rules.error.code), rules.error.message);
        return EXIT_FAILURE;
      }
      arena_core::storage::MySqlConfig mysql{config.data.mysql.uri,
                                             config.data.mysql.user,
                                             environment_value(config.data.mysql.password_env),
                                             config.data.mysql.schema};
      auto repository = std::make_shared<arena_core::storage::MySqlSettlementRepository>(std::move(mysql));
      if (const auto error = repository->check_health()) {
        spdlog::error("{}: {}", arena_core::error_code_name(error.code), error.message);
        return EXIT_FAILURE;
      }
      if (observer_store) {
        observer_store->set_health("mysql", true, "transaction source ready");
      }
      arena_core::rank::RedisRankConfig redis;
      redis.host = config.data.redis.host;
      redis.port = config.data.redis.port;
      redis.database = config.data.redis.database;
      redis.password = environment_value(config.data.redis.password_env);
      redis.key_prefix = config.data.redis.key_prefix;
      redis.connect_timeout_ms = config.data.redis.connect_timeout_ms;
      redis.pool_size = config.execution.storage_threads;
      auto rank_cache = std::make_shared<arena_core::rank::HiredisRankCache>(std::move(redis));
      storage_executor = std::make_shared<arena_core::storage::StorageExecutor>(
          config.execution.storage_threads, config.execution.storage_queue_capacity);
      arena_core::settlement::PersistentSettlementOptions settlement_options;
      settlement_options.repair_interval = std::chrono::milliseconds(config.data.repair_interval_ms);
      settlement_options.repair_batch_size = config.data.repair_batch_size;
      settlement_gateway = std::make_shared<arena_core::settlement::PersistentSettlementService>(
          io_context, std::move(settlement_options), storage_executor, std::move(repository),
          std::move(rank_cache), std::move(*rules.value));
    }
    auto game_service = std::make_shared<arena_core::game::GameService>(
        io_context, arena_core::game::GameServiceOptions{},
        std::make_shared<arena_core::domain::SequentialIdGenerator>(runtime_id_seed()), token_generator,
        std::move(settlement_gateway), observer_store);
    auto server = std::make_shared<arena_core::session::TcpServer>(
        io_context, boost::asio::ip::tcp::endpoint(address, config.network.tcp_port), options,
        token_generator, game_service, observer_store);
    if (const auto error = server->start()) {
      spdlog::error("{}: {}", arena_core::error_code_name(error.code), error.message);
      return EXIT_FAILURE;
    }

    std::shared_ptr<arena_core::observer::ObserverHttpServer> observer_server;
    if (observer_store) {
      arena_core::observer::ObserverHttpConfig observer_config;
      observer_config.bind_address = config.network.bind_address;
      observer_config.port = config.network.observer_port;
      observer_config.static_root = config.observer.static_root;
      observer_config.demo_mode = config.observer.demo_mode;
      observer_server = std::make_shared<arena_core::observer::ObserverHttpServer>(
          io_context, std::move(observer_config), observer_store);
      if (const auto error = observer_server->start()) {
        spdlog::error("{}: {}", arena_core::error_code_name(error.code), error.message);
        return EXIT_FAILURE;
      }
    }

    boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
    signals.async_wait([server, observer_server](const boost::system::error_code&, int) {
      if (observer_server) {
        observer_server->stop();
      }
      server->stop();
    });

    spdlog::info("TCP server listening address={} port={} io_threads={}", config.network.bind_address,
                 server->bound_port(), config.execution.io_threads);
    if (observer_server) {
      spdlog::info("observer listening address={} port={} demo_mode={}", config.network.bind_address,
                   observer_server->bound_port(), config.observer.demo_mode);
    }
    const auto thread_count = std::max<std::size_t>(1, config.execution.io_threads);
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (std::size_t index = 0; index < thread_count; ++index) {
      threads.emplace_back([&io_context] { io_context.run(); });
    }
    for (auto& thread : threads) {
      thread.join();
    }
    if (storage_executor) {
      storage_executor->stop();
    }
    spdlog::info("TCP server stopped");
    return EXIT_SUCCESS;
  } catch (const std::exception&) {
    spdlog::critical("server startup failed");
    return EXIT_FAILURE;
  }
}
