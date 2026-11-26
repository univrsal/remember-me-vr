#include "vrcore/log.h"

#include <cstdio>
#include <cstring>

#include <windows.h>

namespace vrcore::log {
namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
FILE* g_file = nullptr;
Level g_level = Level::Info;
LARGE_INTEGER g_start{};
LARGE_INTEGER g_freq{};

const char* Tag(Level level) {
  switch (level) {
    case Level::Trace: return "TRACE";
    case Level::Debug: return "DEBUG";
    case Level::Info:  return "INFO ";
    case Level::Warn:  return "WARN ";
    case Level::Error: return "ERROR";
    default:           return "?    ";
  }
}

}  // namespace

void Open(const wchar_t* path) {
  AcquireSRWLockExclusive(&g_lock);
  if (!g_file) {
    g_file = _wfopen(path, L"w");
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start);
  }
  ReleaseSRWLockExclusive(&g_lock);
}

void SetLevel(Level level) { g_level = level; }

Level ParseLevel(const char* name, Level fallback) {
  struct { const char* name; Level level; } const table[] = {
      {"trace", Level::Trace}, {"debug", Level::Debug}, {"info", Level::Info},
      {"warn", Level::Warn},   {"error", Level::Error}, {"off", Level::Off},
  };
  for (const auto& entry : table) {
    if (_stricmp(name, entry.name) == 0) return entry.level;
  }
  return fallback;
}

bool Enabled(Level level) { return level >= g_level && g_level != Level::Off; }

void WriteV(Level level, const char* fmt, va_list args) {
  if (!Enabled(level)) return;

  char message[2048];
  vsnprintf(message, sizeof(message), fmt, args);

  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  const double seconds = g_freq.QuadPart ? double(now.QuadPart - g_start.QuadPart) / double(g_freq.QuadPart) : 0.0;

  char line[2200];
  snprintf(line, sizeof(line), "[%10.3f] [%5lu] %s %s\n", seconds, GetCurrentThreadId(), Tag(level), message);

  AcquireSRWLockExclusive(&g_lock);
  if (g_file) {
    fputs(line, g_file);
    fflush(g_file);  // keep the log useful when the game crashes
  }
  ReleaseSRWLockExclusive(&g_lock);
  OutputDebugStringA(line);
}

void Write(Level level, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  WriteV(level, fmt, args);
  va_end(args);
}

}  // namespace vrcore::log
