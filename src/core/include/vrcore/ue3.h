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

  // Struct/property layout (DiscoverTypes), calibrated on Core.Object's own
  // script properties, whose offsets are known from the UObject layout above.
  uint32_t fieldNext = 0;       // UField::Next
  uint32_t structSuper = 0;     // UStruct::SuperField
  uint32_t structChildren = 0;  // UStruct::Children
  uint32_t structSize = 0;      // UStruct::PropertySize
  uint32_t propArrayDim = 0;    // UProperty::ArrayDim
  uint32_t propElementSize = 0; // UProperty::ElementSize
  uint32_t propOffset = 0;      // UProperty::Offset
  uint32_t propClass = 0;       // UObjectProperty::PropertyClass
  uint32_t propStruct = 0;      // UStructProperty::Struct
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

  // Writes "index address vt:+RVA fullname" for every live object (vtable as an
  // exe RVA; "!" marks an absolute address outside the exe). Count, or -1 on error.
  int DumpObjects(const wchar_t* path) const;

  // Detects UField/UStruct/UProperty offsets. Requires Discover().
  bool DiscoverTypes();
  bool TypesValid() const { return layout_.propOffset != 0; }

  // Property lookup: offset of `property` in `className` ("Class Engine.Camera"
  // or "ScriptStruct Core.Object.Vector"), searching super classes too. -1 if missing.
  int32_t PropertyOffset(const std::string& structFullName, const std::string& property) const;

  // Bool property lookup: UE3 packs bools into 32-bit words; `offset` is the
  // word's offset, `mask` the property's bit (UBoolProperty::BitMask, right
  // after the UProperty fields). False if missing or not a BoolProperty.
  bool BoolProperty(const std::string& structFullName, const std::string& property, int32_t& offset,
                    uint32_t& mask) const;

  // Writes every class and script struct with its properties (offset, size,
  // type) and function names. Returns the number of structs, -1 on error.
  int DumpTypes(const wchar_t* path) const;

 private:
  bool FindNames();
  bool FindObjects();
  bool DetectObjectFields(uintptr_t data, int32_t num);
  std::string PropertyType(uintptr_t property) const;

  Layout layout_;
};

}  // namespace vrcore::ue3
