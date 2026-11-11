// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "Log.h"
#include "System.h"
#include <stdarg.h>
#include <stdio.h>

using namespace quantum;

namespace {

// Something logging before Engine::systemInit used to route through System::instance(), which
// aborts with no witness of the caller. Printing the message here names the caller by its own text
// and lets the boot continue to the real failure.
void preInitLog(const char* fmt, va_list arg_ptr){
    fputs("[log before Engine init] ", stdout);
    vprintf(fmt, arg_ptr);
    fputc('\n', stdout);
    fflush(stdout);
}

}

void Log::print(const char *fmt, ...) {
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    if (!System::hasInstance()) { preInitLog(fmt, arg_ptr); va_end(arg_ptr); return; }
    System::instance().platformLog(0, fmt, arg_ptr);
    va_end(arg_ptr);
}

void Log::verbose(const char *fmt, ...) {
	va_list arg_ptr;
	va_start(arg_ptr, fmt);
    if (!System::hasInstance()) { preInitLog(fmt, arg_ptr); va_end(arg_ptr); return; }
    System::instance().platformLog(D_LOG_VERBOSE, fmt, arg_ptr);
	va_end(arg_ptr);
}

void Log::debug(const char *fmt, ...) {
    #ifndef NDEBUG
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    if (!System::hasInstance()) { preInitLog(fmt, arg_ptr); va_end(arg_ptr); return; }
    System::instance().platformLog(D_LOG_DEBUG, fmt, arg_ptr);
    va_end(arg_ptr);
    #endif
}

void Log::warn(const char *fmt, ...) {
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    if (!System::hasInstance()) { preInitLog(fmt, arg_ptr); va_end(arg_ptr); return; }
    System::instance().platformLog(D_LOG_WARN, fmt, arg_ptr);
    va_end(arg_ptr);
}

void Log::error(const char *fmt, ...) {
    va_list arg_ptr;
    va_start(arg_ptr, fmt);
    if (!System::hasInstance()) { preInitLog(fmt, arg_ptr); va_end(arg_ptr); return; }
    System::instance().platformLog(D_LOG_ERROR, fmt, arg_ptr);
    va_end(arg_ptr);
}
