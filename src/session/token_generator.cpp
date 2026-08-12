#include "arena_core/session/token_generator.hpp"

#include <array>
#include <iomanip>
#include <sstream>

namespace arena_core::session {

RandomTokenGenerator::RandomTokenGenerator() = default;

std::string RandomTokenGenerator::next_token() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::array<std::uint32_t, 4> words{};
  for (auto& word : words) {
    word = source_();
  }

  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto word : words) {
    output << std::setw(8) << word;
  }
  return output.str();
}

std::string DeterministicTokenGenerator::next_token() {
  std::ostringstream output;
  output << "test-token-" << std::setfill('0') << std::setw(8) << ++next_;
  return output.str();
}

}  // namespace arena_core::session
