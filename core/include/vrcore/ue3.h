#pragma once

#include <cstdint>
#include <string>

namespace vrcore::ue3 {

// UE3 runtime reflection, discovered by shape rather than game-specific
// signatures, so it carries over to other 32-bit UE3 games:
//  - GNames:   TArray<FNameEntry*> in the exe's writable data; entry 0 is "None",
//              entry 1 "ByteProperty".
//  - GObjects: TArray<UObject*> whose objects store their own array index.
// Field offsets inside FNameEntry/UObject are detected and logged.
struct Layout {
  uintptr_t namesArray = 0;     // address of the GNames TArray header
  uintptr_t objectsArray = 0;   // address of the GObjects TArray header
  uint32_t nameString = 0;      // FNameEntry: offset of the ANSI string
  uint32_t objIndex = 0;        // UObject::Index
  uint32_t objOuter = 0;        // UObject::Outer
  uint32_t objName = 0;         // UObject::Name (FName: int32 index, int32 number)
  uint32_t objClass = 0;        // UObject::Class
};

class Reflection {
 public:
  // Scans the host exe. Call once the engine is initialised (e.g. from
  // Game::OnDeviceCreated). Logs what it finds; false if GNames/GObjects weren't found.
  bool Discover();
  bool Valid() const { return layout_.objClass != 0; }
  const Layout& GetLayout() const { return layout_; }

  int32_t NameCount() const;
  std::string Name(int32_t index, int32_t number = 0) const;  // "" if invalid

  int32_t ObjectCount() const;
  uintptr_t Object(int32_t index) const;  // 0 if empty/invalid
  std::string ObjectName(uintptr_t object) const;
  std::string ClassName(uintptr_t object) const;
  // "Class Outermost.Outer.Name", the UE3 GetFullName() format.
  std::string FullName(uintptr_t object) const;
  uintptr_t FindObject(const std::string& fullName) const;

  // Writes "index address fullname" for every live object. Returns the count, -1 on error.
  int DumpObjects(const wchar_t* path) const;

 private:
  bool FindNames();
  bool FindObjects();
  bool DetectObjectFields(uintptr_t data, int32_t num);

  Layout layout_;
};

}  // namespace vrcore::ue3
