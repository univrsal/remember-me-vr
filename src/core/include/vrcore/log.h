#pragma once

#include <cstdarg>

namespace vrcore::log {

enum class Level { Trace = 0, Debug, Info, Warn, Error, Off };

// Opens (truncates) the log file. Safe to call once; later calls are ignored.
void Open(const wchar_t* path);
void SetLevel(Level level);
Level ParseLevel(const char* name, Level fallback);
bool Enabled(Level level);

void Write(Level level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void WriteV(Level level, const char* fmt, va_list args);

}  // namespace vrcore::log

#define VR_LOG_TRACE(...) ::vrcore::log::Write(::vrcore::log::Level::Trace, __VA_ARGS__)
#define VR_LOG_DEBUG(...) ::vrcore::log::Write(::vrcore::log::Level::Debug, __VA_ARGS__)
#define VR_LOG_INFO(...)  ::vrcore::log::Write(::vrcore::log::Level::Info,  __VA_ARGS__)
#define VR_LOG_WARN(...)  ::vrcore::log::Write(::vrcore::log::Level::Warn,  __VA_ARGS__)
#define VR_LOG_ERROR(...) ::vrcore::log::Write(::vrcore::log::Level::Error, __VA_ARGS__)
