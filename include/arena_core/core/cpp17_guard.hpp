#pragma once

#if defined(_MSVC_LANG)
#define ARENA_CORE_CXX_LANGUAGE_LEVEL _MSVC_LANG
#else
#define ARENA_CORE_CXX_LANGUAGE_LEVEL __cplusplus
#endif

#if ARENA_CORE_CXX_LANGUAGE_LEVEL < 201703L || ARENA_CORE_CXX_LANGUAGE_LEVEL >= 202002L
#error "arena-core-server requires exactly C++17"
#endif

namespace arena_core {

inline constexpr long kCxxLanguageLevel = ARENA_CORE_CXX_LANGUAGE_LEVEL;

}  // namespace arena_core
