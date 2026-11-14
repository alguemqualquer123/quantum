// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef QUANTUM_EXPORT_H
#define QUANTUM_EXPORT_H

#if defined(_WIN32)
    #ifdef QUANTUM_SHARED
        #ifdef QUANTUM_EXPORTS
            #define QUANTUM_API __declspec(dllexport)
        #else
            #define QUANTUM_API __declspec(dllimport)
        #endif
    #else
        #define QUANTUM_API
    #endif
#else
    #define QUANTUM_API
#endif

#endif // QUANTUM_EXPORT_H