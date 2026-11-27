#include "vrcore/config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include <windows.h>

namespace vrcore {
namespace {

std::wstring Widen(const char* s) {
  std::wstring out;
  while (*s) out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*s++)));
  return out;
}

}  // namespace

bool Config::Exists() const {
  return !path_.empty() && GetFileAttributesW(path_.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::string Config::GetString(const char* section, const char* key, const char* fallback) const {
  if (path_.empty()) return fallback;
  wchar_t buffer[1024];
  const std::wstring wfallback = Widen(fallback);
  GetPrivateProfileStringW(Widen(section).c_str(), Widen(key).c_str(), wfallback.c_str(), buffer,
                           static_cast<DWORD>(std::size(buffer)), path_.c_str());
  // Values are ASCII in practice; strip inline "; comments" and trailing spaces.
  std::string out;
  for (const wchar_t* p = buffer; *p && *p != L';'; ++p) out.push_back(static_cast<char>(*p));
  while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
  return out;
}

int Config::GetInt(const char* section, const char* key, int fallback) const {
  const std::string value = GetString(section, key, "");
  if (value.empty()) return fallback;
  char* end = nullptr;
  const long parsed = strtol(value.c_str(), &end, 0);
  return (end && *end == '\0') ? static_cast<int>(parsed) : fallback;
}

float Config::GetFloat(const char* section, const char* key, float fallback) const {
  const std::string value = GetString(section, key, "");
  if (value.empty()) return fallback;
  char* end = nullptr;
  const float parsed = strtof(value.c_str(), &end);
  return (end && *end == '\0') ? parsed : fallback;
}

bool Config::GetBool(const char* section, const char* key, bool fallback) const {
  const std::string value = GetString(section, key, "");
  if (value.empty()) return fallback;
  for (const char* t : {"1", "true", "yes", "on"}) {
    if (_stricmp(value.c_str(), t) == 0) return true;
  }
  for (const char* f : {"0", "false", "no", "off"}) {
    if (_stricmp(value.c_str(), f) == 0) return false;
  }
  return fallback;
}

bool Config::Set(const char* section, const char* key, const std::string& value) const {
  if (path_.empty()) return false;
  return WritePrivateProfileStringW(Widen(section).c_str(), Widen(key).c_str(), Widen(value.c_str()).c_str(),
                                    path_.c_str()) != 0;
}

bool Config::SetInt(const char* section, const char* key, int value, bool hex) const {
  char buf[32];
  std::snprintf(buf, sizeof(buf), hex ? "0x%02X" : "%d", value);
  return Set(section, key, buf);
}

bool Config::SetFloat(const char* section, const char* key, float value) const {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%g", value);
  return Set(section, key, buf);
}

bool Config::SetBool(const char* section, const char* key, bool value) const {
  return Set(section, key, value ? "1" : "0");
}

}  // namespace vrcore
