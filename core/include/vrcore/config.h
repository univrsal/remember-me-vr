#pragma once

#include <string>

namespace vrcore {

// Read-only INI access (GetPrivateProfile* under the hood, works the same under Wine).
class Config {
 public:
  explicit Config(std::wstring path = {}) : path_(std::move(path)) {}

  const std::wstring& Path() const { return path_; }
  bool Exists() const;

  std::string GetString(const char* section, const char* key, const char* fallback) const;
  int GetInt(const char* section, const char* key, int fallback) const;
  float GetFloat(const char* section, const char* key, float fallback) const;
  bool GetBool(const char* section, const char* key, bool fallback) const;

 private:
  std::wstring path_;
};

}  // namespace vrcore
