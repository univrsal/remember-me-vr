// UField / UStruct / UProperty layout discovery and the class/property dump.

#include <algorithm>
#include <cstdio>
#include <set>
#include <unordered_map>
#include <vector>

#include "vrcore/log.h"
#include "vrcore/memory.h"
#include "vrcore/ue3.h"

namespace vrcore::ue3 {
namespace {

constexpr uint32_t kScanFrom = 0x3C;  // first byte after the 32-bit UE3 UObject
constexpr uint32_t kScanTo = 0xC0;

uintptr_t ReadPtr(uintptr_t at) {
  uint32_t v = 0;
  return mem::Read(at, v) ? v : 0;
}

int32_t ReadI32(uintptr_t at) {
  int32_t v = 0;
  return mem::Read(at, v) ? v : INT32_MIN;
}

bool EndsWith(const std::string& s, const char* suffix) {
  const size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

}  // namespace

bool Reflection::DiscoverTypes() {
  if (!Valid()) return false;
  Layout& l = layout_;

  // One pass over GObjects for the calibration objects.
  std::unordered_map<std::string, uintptr_t> want = {
      {"Class Core.Object", 0},
      {"Class Core.Class", 0},
      {"Class Engine.Actor", 0},
      {"Class Engine.Pawn", 0},
      {"ObjectProperty Core.Object.Outer", 0},
      {"NameProperty Core.Object.Name", 0},
      {"ObjectProperty Core.Object.Class", 0},
      {"StructProperty Engine.Actor.Location", 0},
      {"ScriptStruct Core.Object.Vector", 0},
  };
  const std::set<std::string> classes = {"Class", "ObjectProperty", "NameProperty", "StructProperty", "ScriptStruct"};
  const int32_t count = ObjectCount();
  for (int32_t i = 0; i < count; ++i) {
    const uintptr_t object = Object(i);
    if (!object || !classes.count(ClassName(object))) continue;
    auto it = want.find(FullName(object));
    if (it != want.end() && !it->second) it->second = object;
  }
  for (const auto& [name, object] : want) {
    if (!object) {
      VR_LOG_ERROR("ue3: type discovery needs %s, not found", name.c_str());
      return false;
    }
  }
  const uintptr_t coreObject = want["Class Core.Object"], coreClass = want["Class Core.Class"];
  const uintptr_t actor = want["Class Engine.Actor"], pawn = want["Class Engine.Pawn"];
  const uintptr_t pOuter = want["ObjectProperty Core.Object.Outer"];
  const uintptr_t pName = want["NameProperty Core.Object.Name"];
  const uintptr_t pClass = want["ObjectProperty Core.Object.Class"];
  const uintptr_t pLocation = want["StructProperty Engine.Actor.Location"];
  const uintptr_t vectorStruct = want["ScriptStruct Core.Object.Vector"];

  auto firstOffset = [](auto&& predicate) -> uint32_t {
    for (uint32_t off = kScanFrom; off <= kScanTo; off += 4) {
      if (predicate(off)) return off;
    }
    return 0;
  };

  // UProperty::Offset: Core.Object's script properties mirror the UObject fields.
  l.propOffset = firstOffset([&](uint32_t o) {
    return ReadI32(pOuter + o) == static_cast<int32_t>(l.objOuter) &&
           ReadI32(pName + o) == static_cast<int32_t>(l.objName) &&
           ReadI32(pClass + o) == static_cast<int32_t>(l.objClass);
  });
  // ElementSize: pointer 4, FName 8.
  l.propElementSize = firstOffset([&](uint32_t o) {
    return ReadI32(pOuter + o) == 4 && ReadI32(pName + o) == 8 && ReadI32(pClass + o) == 4;
  });
  // ArrayDim: 1 for all three; UE3 stores it right before ElementSize.
  auto dimOk = [&](uint32_t o) { return ReadI32(pOuter + o) == 1 && ReadI32(pName + o) == 1 && ReadI32(pClass + o) == 1; };
  l.propArrayDim = (l.propElementSize >= 4 && dimOk(l.propElementSize - 4)) ? l.propElementSize - 4 : firstOffset(dimOk);
  // ObjectProperty::PropertyClass and StructProperty::Struct.
  l.propClass = firstOffset([&](uint32_t o) { return ReadPtr(pOuter + o) == coreObject && ReadPtr(pClass + o) == coreClass; });
  l.propStruct = firstOffset([&](uint32_t o) { return ReadPtr(pLocation + o) == vectorStruct; });
  // UStruct::SuperField: Actor -> Object and Pawn -> Actor.
  l.structSuper = firstOffset([&](uint32_t o) { return ReadPtr(actor + o) == coreObject && ReadPtr(pawn + o) == actor; });

  // UStruct::Children + UField::Next: the chain from Core.Object must reach
  // Outer, Name and Class, with every link owned by Core.Object.
  for (uint32_t ch = kScanFrom; ch <= kScanTo && !l.structChildren; ch += 4) {
    const uintptr_t first = ReadPtr(coreObject + ch);
    if (!first || ReadPtr(first + l.objOuter) != coreObject) continue;
    for (uint32_t nx = kScanFrom; nx <= 0x60; nx += 4) {
      int hits = 0;
      uintptr_t field = first;
      for (int steps = 0; field && steps < 1000; ++steps) {
        if (ReadPtr(field + l.objOuter) != coreObject) break;
        hits += field == pOuter || field == pName || field == pClass;
        field = ReadPtr(field + nx);
      }
      if (hits == 3) {
        l.structChildren = ch;
        l.fieldNext = nx;
        break;
      }
    }
  }

  // UStruct::PropertySize: Core.Object's size equals the end of its last property.
  int32_t objectSize = 0;
  if (l.structChildren && l.propOffset && l.propElementSize) {
    for (uintptr_t f = ReadPtr(coreObject + l.structChildren); f; f = ReadPtr(f + l.fieldNext)) {
      if (!EndsWith(ClassName(f), "Property")) continue;
      const int32_t dim = l.propArrayDim ? std::max(1, ReadI32(f + l.propArrayDim)) : 1;
      objectSize = std::max(objectSize, ReadI32(f + l.propOffset) + ReadI32(f + l.propElementSize) * dim);
    }
    l.structSize = firstOffset([&](uint32_t o) {
      return ReadI32(coreObject + o) == objectSize && ReadI32(actor + o) > objectSize && ReadI32(pawn + o) > ReadI32(actor + o);
    });
  }

  VR_LOG_INFO("ue3: UField Next +0x%x; UStruct Super +0x%x Children +0x%x Size +0x%x (UObject = 0x%x)", l.fieldNext,
              l.structSuper, l.structChildren, l.structSize, objectSize);
  VR_LOG_INFO("ue3: UProperty ArrayDim +0x%x ElementSize +0x%x Offset +0x%x; PropertyClass +0x%x; Struct +0x%x",
              l.propArrayDim, l.propElementSize, l.propOffset, l.propClass, l.propStruct);
  const bool ok = l.fieldNext && l.structSuper && l.structChildren && l.structSize && l.propArrayDim &&
                  l.propElementSize && l.propOffset;
  if (!ok) {
    VR_LOG_ERROR("ue3: type layout incomplete; class dump disabled");
    l.propOffset = 0;
    return false;
  }
  VR_LOG_INFO("ue3: sanity check: Actor.Location at +0x%x", PropertyOffset("Class Engine.Actor", "Location"));
  return true;
}

int32_t Reflection::PropertyOffset(const std::string& structFullName, const std::string& property) const {
  if (!TypesValid()) return -1;
  for (uintptr_t s = FindObject(structFullName); s; s = ReadPtr(s + layout_.structSuper)) {
    for (uintptr_t f = ReadPtr(s + layout_.structChildren); f; f = ReadPtr(f + layout_.fieldNext)) {
      if (ObjectName(f) == property && EndsWith(ClassName(f), "Property")) return ReadI32(f + layout_.propOffset);
    }
  }
  return -1;
}

std::string Reflection::PropertyType(uintptr_t property) const {
  const std::string cls = ClassName(property);
  if (cls == "IntProperty") return "int";
  if (cls == "FloatProperty") return "float";
  if (cls == "BoolProperty") return "bool";
  if (cls == "ByteProperty") return "byte";
  if (cls == "NameProperty") return "FName";
  if (cls == "StrProperty") return "FString";
  if (cls == "StructProperty" && layout_.propStruct) return "struct " + ObjectName(ReadPtr(property + layout_.propStruct));
  if ((cls == "ObjectProperty" || cls == "ComponentProperty" || cls == "ClassProperty") && layout_.propClass) {
    return "U" + ObjectName(ReadPtr(property + layout_.propClass)) + "*";
  }
  if (cls == "ArrayProperty") {
    // The inner property is a sub-object of the array property.
    for (uint32_t o = layout_.propOffset + 4; o <= layout_.propOffset + 0x40; o += 4) {
      const uintptr_t inner = ReadPtr(property + o);
      if (inner && ReadPtr(inner + layout_.objOuter) == property && EndsWith(ClassName(inner), "Property")) {
        return "TArray<" + PropertyType(inner) + ">";
      }
    }
    return "TArray<?>";
  }
  return cls;
}

int Reflection::DumpTypes(const wchar_t* path) const {
  if (!TypesValid()) return -1;
  FILE* file = _wfopen(path, L"w");
  if (!file) return -1;
  const Layout& l = layout_;
  struct Prop {
    int32_t offset, size, dim;
    std::string type, name;
  };
  int structs = 0;
  const int32_t count = ObjectCount();
  for (int32_t i = 0; i < count; ++i) {
    const uintptr_t s = Object(i);
    if (!s) continue;
    const std::string kind = ClassName(s);
    if (kind != "Class" && kind != "ScriptStruct") continue;

    std::vector<Prop> props;
    std::vector<std::string> functions;
    int steps = 0;
    for (uintptr_t f = ReadPtr(s + l.structChildren); f && steps < 4000; f = ReadPtr(f + l.fieldNext), ++steps) {
      const std::string fieldClass = ClassName(f);
      if (EndsWith(fieldClass, "Property")) {
        props.push_back({ReadI32(f + l.propOffset), ReadI32(f + l.propElementSize), ReadI32(f + l.propArrayDim),
                         PropertyType(f), ObjectName(f)});
      } else if (fieldClass == "Function") {
        functions.push_back(ObjectName(f));
      }
    }
    std::sort(props.begin(), props.end(), [](const Prop& a, const Prop& b) { return a.offset < b.offset; });

    const uintptr_t super = ReadPtr(s + l.structSuper);
    std::fprintf(file, "%s : %s  // size 0x%X\n", FullName(s).c_str(), super ? FullName(super).c_str() : "-",
                 ReadI32(s + l.structSize));
    for (const auto& p : props) {
      std::fprintf(file, "  0x%04X %-32s %s%s  // 0x%X\n", p.offset, p.type.c_str(), p.name.c_str(),
                   p.dim > 1 ? ("[" + std::to_string(p.dim) + "]").c_str() : "", p.size * std::max(1, p.dim));
    }
    for (const auto& fn : functions) std::fprintf(file, "  fn %s\n", fn.c_str());
    std::fputc('\n', file);
    ++structs;
  }
  std::fclose(file);
  return structs;
}

}  // namespace vrcore::ue3
