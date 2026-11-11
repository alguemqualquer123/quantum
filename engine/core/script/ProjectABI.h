// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef QUANTUM_PROJECT_ABI_H
#define QUANTUM_PROJECT_ABI_H

#include <cstdint>

namespace quantum {

// This value is exchanged through a C function before the editor calls any
// generated C++ script entry point. It catches the ABI mismatches that the
// linker cannot detect when a plugin is loaded dynamically.
constexpr uint64_t projectAbiFingerprint() noexcept {
    uint64_t value = 0x5155414E54554D31ULL;
    value = (value * 131ULL) ^ static_cast<uint64_t>(sizeof(void*));
#if defined(_MSC_VER)
    value = (value * 131ULL) ^ static_cast<uint64_t>(_MSC_VER);
#elif defined(__clang__)
    value = (value * 131ULL) ^ static_cast<uint64_t>(__clang_major__ * 10000
        + __clang_minor__ * 100 + __clang_patchlevel__);
#elif defined(__GNUC__)
    value = (value * 131ULL) ^ static_cast<uint64_t>(__GNUC__ * 10000
        + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__);
#else
    value = (value * 131ULL) ^ 0xFFFFFFFFULL;
#endif
#if defined(_ITERATOR_DEBUG_LEVEL)
    value = (value * 131ULL) ^ static_cast<uint64_t>(_ITERATOR_DEBUG_LEVEL);
#else
    value = (value * 131ULL) ^ 0ULL;
#endif
#if defined(_DEBUG)
    value = (value * 131ULL) ^ 1ULL;
#else
    value = (value * 131ULL) ^ 0ULL;
#endif
#if defined(QUANTUM_EDITOR)
    value = (value * 131ULL) ^ 1ULL;
#else
    value = (value * 131ULL) ^ 0ULL;
#endif
#if defined(QUANTUM_SHARED)
    value = (value * 131ULL) ^ 1ULL;
#else
    value = (value * 131ULL) ^ 0ULL;
#endif
    return value;
}

}

#endif
