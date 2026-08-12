#include "arena_core/core/cpp17_guard.hpp"

int main() { return arena_core::kCxxLanguageLevel == 201703L ? 0 : 1; }
