#pragma once

// The engine's log. Each platform installs a sink once (logcat, os_log); until then
// lines go to stderr, which is what the desktop tests and tools want.
namespace splatkit {

enum class LogLevel { info, warn, error };

using LogSink = void (*)(LogLevel level, const char* message);
void setLogSink(LogSink sink);

// printf style, one line per call.
void logf(LogLevel level, const char* format, ...) __attribute__((format(printf, 2, 3)));

}  // namespace splatkit

#define LOGI(...) ::splatkit::logf(::splatkit::LogLevel::info, __VA_ARGS__)
#define LOGW(...) ::splatkit::logf(::splatkit::LogLevel::warn, __VA_ARGS__)
#define LOGE(...) ::splatkit::logf(::splatkit::LogLevel::error, __VA_ARGS__)
