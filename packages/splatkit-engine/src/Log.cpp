#include "splatkit/Log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace splatkit {
namespace {

void stderrSink(LogLevel level, const char* message) {
  const char* tag = level == LogLevel::error ? "E" : level == LogLevel::warn ? "W" : "I";
  std::fprintf(stderr, "SplatKit %s: %s\n", tag, message);
}

std::atomic<LogSink> gSink{&stderrSink};

}  // namespace

void setLogSink(LogSink newSink) {
  gSink.store(newSink != nullptr ? newSink : &stderrSink);
}

void logf(LogLevel level, const char* format, ...) {
  char line[1024];
  va_list args;
  va_start(args, format);
  std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  gSink.load()(level, line);
}

}  // namespace splatkit
