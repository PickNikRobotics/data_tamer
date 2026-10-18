#pragma once

/// Hidden visibility for the inline `stats()` functions: each binary must use the copy
/// compiled against its own header, because another loaded library's copy may be built
/// for an older `Stats` that has since grown. See CLAUDE.md, "Invariants".
#if defined(__GNUC__) || defined(__clang__)
#define DATA_TAMER_INLINE_LOCAL __attribute__((visibility("hidden")))
#else
#define DATA_TAMER_INLINE_LOCAL
#endif

/// Marks the real-time API `[[clang::nonblocking]]` (Clang 20 and later), so that a
/// caller marked `[[clang::nonblocking]]` is checked by -Wfunction-effects at compile
/// time and by RealtimeSanitizer (-fsanitize=realtime) at run time. Nothing elsewhere.
/// The attribute is not part of a mangled name, so it changes no symbol.
#if defined(__clang__) && defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::nonblocking)
#define DATA_TAMER_HAS_NONBLOCKING 1
#define DATA_TAMER_NONBLOCKING [[clang::nonblocking]]
#endif
#endif
#if !defined(DATA_TAMER_HAS_NONBLOCKING)
#define DATA_TAMER_NONBLOCKING
#endif
