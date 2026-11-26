#include <cstdio>
#include <vector>

#include "vrcore/log.h"
#include "vrcore/memory.h"
#include "vrcore/process.h"
#include "vrcore/ue3.h"

namespace vrcore::ue3 {
namespace {

struct RawArray {
  uintptr_t data;
  int32_t num;
  int32_t max;
};

bool ReadArray(uintptr_t at, RawArray& out) { return at && mem::Read(at, out); }

uintptr_t ReadPtr(uintptr_t at) {
  uint32_t v = 0;
  return mem::Read(at, v) ? v : 0;
}

int32_t ReadI32(uintptr_t at, bool& ok) {
  int32_t v = 0;
  ok = mem::Read(at, v);
  return v;
}

}  // namespace

bool Reflection::Discover() {
  layout_ = {};
  if (!FindNames()) {
    VR_LOG_ERROR("ue3: GNames not found");
    return false;
  }
  if (!FindObjects()) {
    VR_LOG_ERROR("ue3: GObjects not found");
    return false;
  }
  const auto& l = layout_;
  VR_LOG_INFO("ue3: GNames @ %08x (%d names, string at +0x%x)", static_cast<unsigned>(l.namesArray), NameCount(),
              l.nameString);
  VR_LOG_INFO("ue3: GObjects @ %08x (%d slots); UObject Index +0x%x Outer +0x%x Name +0x%x Class +0x%x",
              static_cast<unsigned>(l.objectsArray), ObjectCount(), l.objIndex, l.objOuter, l.objName, l.objClass);
  const uintptr_t objectClass = FindObject("Class Core.Object");
  VR_LOG_INFO("ue3: sanity check: \"Class Core.Object\" %s", objectClass ? "found" : "NOT found");
  return true;
}

bool Reflection::FindNames() {
  for (const auto& section : mem::ExeSections()) {
    if (!section.writable) continue;
    for (uintptr_t p = section.start; p + sizeof(RawArray) <= section.start + section.size; p += 4) {
      RawArray a;
      if (!ReadArray(p, a) || a.num < 1000 || a.num > a.max || a.max > 4000000) continue;
      const uintptr_t first = ReadPtr(a.data);
      const uintptr_t second = ReadPtr(a.data + 4);
      if (!first || !second) continue;
      for (uint32_t off = 0; off <= 0x20; off += 4) {
        std::string s0, s1;
        if (mem::ReadCString(first + off, s0, 8) && s0 == "None" && mem::ReadCString(second + off, s1, 16) &&
            s1 == "ByteProperty") {
          layout_.namesArray = p;
          layout_.nameString = off;
          return true;
        }
      }
    }
  }
  return false;
}

bool Reflection::FindObjects() {
  for (const auto& section : mem::ExeSections()) {
    if (!section.writable) continue;
    for (uintptr_t p = section.start; p + sizeof(RawArray) <= section.start + section.size; p += 4) {
      RawArray a;
      if (p == layout_.namesArray || !ReadArray(p, a) || a.num < 1000 || a.num > a.max || a.max > 8000000) continue;
      if (!mem::IsReadable(reinterpret_cast<const void*>(a.data), 256 * 4)) continue;
      layout_.objectsArray = p;
      if (DetectObjectFields(a.data, a.num)) return true;
      layout_.objectsArray = 0;
    }
  }
  return false;
}

bool Reflection::DetectObjectFields(uintptr_t data, int32_t num) {
  struct Sample {
    int32_t index;
    uintptr_t object;
  };
  std::vector<Sample> samples;
  for (int32_t i = 0; i < num && samples.size() < 256; ++i) {
    const uintptr_t object = ReadPtr(data + 4u * i);
    if (object) samples.push_back({i, object});
  }
  if (samples.size() < 64) return false;

  // UObject::Index: the field equal to the object's own array index.
  for (uint32_t k = 4; k <= 0x60 && !layout_.objIndex; k += 4) {
    bool all = true;
    for (const auto& s : samples) {
      bool ok;
      if (ReadI32(s.object + k, ok) != s.index || !ok) {
        all = false;
        break;
      }
    }
    if (all) layout_.objIndex = k;
  }
  if (!layout_.objIndex) return false;

  auto isObject = [&](uintptr_t object) {
    bool ok;
    const int32_t index = ReadI32(object + layout_.objIndex, ok);
    return ok && index >= 0 && index < num && ReadPtr(data + 4u * index) == object;
  };
  const int32_t names = NameCount();
  auto validName = [&](uintptr_t at) {
    bool ok1, ok2;
    const int32_t index = ReadI32(at, ok1);
    const int32_t number = ReadI32(at + 4, ok2);
    return ok1 && ok2 && index >= 0 && index < names && number >= 0 && number < 100000 && !Name(index).empty();
  };

  const uint32_t lo = layout_.objIndex + 4, hi = layout_.objIndex + 0x40;
  for (uint32_t c = lo; c <= hi; c += 4) {  // UObject::Class
    bool classOk = true;
    for (const auto& s : samples) {
      const uintptr_t cls = ReadPtr(s.object + c);
      if (!cls || !isObject(cls) || !isObject(ReadPtr(cls + c))) {
        classOk = false;
        break;
      }
    }
    if (!classOk) continue;
    for (uint32_t n = lo; n <= hi; n += 4) {  // UObject::Name
      if (n == c || n + 4 == c) continue;
      bool nameOk = true;
      for (const auto& s : samples) {
        if (!validName(s.object + n)) {
          nameOk = false;
          break;
        }
      }
      if (!nameOk) continue;
      // The class of a class object is named "Class".
      layout_.objClass = c;
      layout_.objName = n;
      const uintptr_t metaClass = ReadPtr(ReadPtr(samples[0].object + c) + c);
      if (ObjectName(metaClass) != "Class") {
        layout_.objClass = layout_.objName = 0;
        continue;
      }
      // UObject::Outer: null or an object for every sample, non-null for some.
      for (uint32_t o = lo; o <= hi; o += 4) {
        if (o == c || o == n || o == n + 4) continue;
        bool outerOk = true, anyNonNull = false;
        for (const auto& s : samples) {
          const uintptr_t outer = ReadPtr(s.object + o);
          if (outer && !isObject(outer)) {
            outerOk = false;
            break;
          }
          anyNonNull |= outer != 0;
        }
        if (outerOk && anyNonNull) {
          layout_.objOuter = o;
          return true;
        }
      }
      layout_.objClass = layout_.objName = 0;
    }
  }
  layout_.objIndex = 0;
  return false;
}

int32_t Reflection::NameCount() const {
  RawArray a;
  return ReadArray(layout_.namesArray, a) ? a.num : 0;
}

std::string Reflection::Name(int32_t index, int32_t number) const {
  RawArray a;
  if (!ReadArray(layout_.namesArray, a) || index < 0 || index >= a.num) return {};
  const uintptr_t entry = ReadPtr(a.data + 4u * index);
  std::string s;
  if (!entry || !mem::ReadCString(entry + layout_.nameString, s, 1024)) return {};
  if (number > 0) s += "_" + std::to_string(number - 1);
  return s;
}

int32_t Reflection::ObjectCount() const {
  RawArray a;
  return ReadArray(layout_.objectsArray, a) ? a.num : 0;
}

uintptr_t Reflection::Object(int32_t index) const {
  RawArray a;
  if (!ReadArray(layout_.objectsArray, a) || index < 0 || index >= a.num) return 0;
  return ReadPtr(a.data + 4u * index);
}

std::string Reflection::ObjectName(uintptr_t object) const {
  if (!object || !layout_.objName) return {};
  bool ok1, ok2;
  const int32_t index = ReadI32(object + layout_.objName, ok1);
  const int32_t number = ReadI32(object + layout_.objName + 4, ok2);
  return ok1 && ok2 ? Name(index, number) : std::string();
}

std::string Reflection::ClassName(uintptr_t object) const {
  return object && layout_.objClass ? ObjectName(ReadPtr(object + layout_.objClass)) : std::string();
}

std::string Reflection::FullName(uintptr_t object) const {
  if (!object) return {};
  std::string path = ObjectName(object);
  uintptr_t outer = layout_.objOuter ? ReadPtr(object + layout_.objOuter) : 0;
  for (int depth = 0; outer && depth < 32; ++depth) {
    path = ObjectName(outer) + "." + path;
    outer = ReadPtr(outer + layout_.objOuter);
  }
  return ClassName(object) + " " + path;
}

uintptr_t Reflection::FindObject(const std::string& fullName) const {
  const int32_t count = ObjectCount();
  for (int32_t i = 0; i < count; ++i) {
    const uintptr_t object = Object(i);
    if (object && FullName(object) == fullName) return object;
  }
  return 0;
}

int Reflection::DumpObjects(const wchar_t* path) const {
  if (!Valid()) return -1;
  FILE* file = _wfopen(path, L"w");
  if (!file) return -1;
  const int32_t count = ObjectCount();
  int written = 0;
  for (int32_t i = 0; i < count; ++i) {
    const uintptr_t object = Object(i);
    if (!object) continue;
    // vtable as an RVA of the exe (maps onto Ghidra addresses: preferred base + RVA).
    const uintptr_t vtable = ReadPtr(object);
    const uintptr_t base = reinterpret_cast<uintptr_t>(Exe().base);
    const bool inExe = vtable >= base && vtable < base + Exe().imageSize;
    std::fprintf(file, "%6d %08x vt:%c%08x %s\n", i, static_cast<unsigned>(object), inExe ? '+' : '!',
                 static_cast<unsigned>(inExe ? vtable - base : vtable), FullName(object).c_str());
    ++written;
  }
  std::fclose(file);
  return written;
}

}  // namespace vrcore::ue3
