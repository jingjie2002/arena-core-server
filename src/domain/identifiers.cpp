#include "arena_core/domain/identifiers.hpp"

#include <cctype>
#include <iomanip>
#include <sstream>

namespace arena_core::domain {
namespace {

bool allowed_identifier_character(const char character) noexcept {
  const auto value = static_cast<unsigned char>(character);
  return std::isalnum(value) != 0 || character == '_' || character == '-' || character == ':' || character == '.';
}

bool valid_identifier(const std::string_view value, const std::size_t maximum) noexcept {
  if (value.empty() || value.size() > maximum) {
    return false;
  }
  for (const char character : value) {
    if (!allowed_identifier_character(character)) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool is_valid_player_id(const std::string_view player_id) noexcept { return valid_identifier(player_id, 64); }

bool is_valid_resource_id(const std::string_view resource_id) noexcept { return valid_identifier(resource_id, 96); }

std::string SequentialIdGenerator::next(const std::string_view prefix) {
  const auto value = sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
  std::ostringstream output;
  output << prefix << '-' << std::setfill('0') << std::setw(8) << value;
  return output.str();
}

}  // namespace arena_core::domain
