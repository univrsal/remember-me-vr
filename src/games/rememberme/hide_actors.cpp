#include "hide_actors.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <unordered_set>
#include <vector>

#include "engine.h"
#include "vrcore/log.h"
#include "vrcore/memory.h"

namespace rm::hide_actors {
namespace {

std::atomic<bool> g_hidePlayer{false};
std::atomic<bool> g_hideNpcs{false};
std::atomic<bool> g_hideSkeletal{false};
std::atomic<bool> g_ready{false};

// From reflection (Init, render thread; read on the game thread after g_ready).
uintptr_t g_objectsArray = 0;  // GObjects TArray header
uint32_t g_objIndex = 0, g_objClass = 0, g_structSuper = 0;
uintptr_t g_pawnClass = 0, g_actorClass = 0;
uintptr_t g_crowdClass = 0;         // Engine.CrowdAgentBase (crowd NPCs; not pawns), 0 if absent
uintptr_t g_skeletalActorClass = 0;  // Engine.SkeletalMeshActor (placed / animated characters and props)
uint32_t g_skeletalActorComponent = 0x264;  // SkeletalMeshActor.SkeletalMeshComponent
int32_t g_componentMesh = -1;               // SkeletalMeshComponent.SkeletalMesh
const vrcore::ue3::Reflection* g_reflection = nullptr;  // names for the log
uint32_t g_controllerPawn = 0x264;     // Controller.Pawn
uint32_t g_hiddenActors = 0x604;       // PlayerController.HiddenActors (TArray)
uint32_t g_actorOwner = 0x98, g_actorBase = 0x9C;

struct Entry {
  uintptr_t object;
  int32_t index;  // UObject::Index: still in GObjects at that slot = still alive
};

// Game thread.
std::vector<Entry> g_player, g_npcs, g_skeletal;
size_t g_loggedSkeletal = static_cast<size_t>(-1);
int g_skeletalLogs = 0;
uintptr_t g_scannedPawn = 0;
uint64_t g_nextScanMs = 0;
std::vector<uint32_t> g_buffer;  // swapped-in HiddenActors data
constexpr size_t kCapacity = 4096;
bool g_loggedScan = false;

template <typename T>
T Raw(uintptr_t address) {
  return *reinterpret_cast<const T*>(address);
}

bool IsA(uintptr_t object, uintptr_t cls) {
  for (uintptr_t c = Raw<uintptr_t>(object + g_objClass); c; c = Raw<uintptr_t>(c + g_structSuper)) {
    if (c == cls) return true;
  }
  return false;
}

bool Alive(const Entry& e) {
  uintptr_t data = 0;
  int32_t num = 0;
  vrcore::mem::Read(g_objectsArray, data);
  vrcore::mem::Read(g_objectsArray + 4, num);
  return data && e.index >= 0 && e.index < num && Raw<uintptr_t>(data + 4u * e.index) == e.object;
}

uintptr_t PlayerPawn(void* localPlayer) {
  uintptr_t controller = 0, pawn = 0;
  if (!vrcore::mem::Read(reinterpret_cast<uintptr_t>(localPlayer) + kLocalPlayerActor, controller) || !controller) {
    return 0;
  }
  vrcore::mem::Read(controller + g_controllerPawn, pawn);
  return pawn;
}

// The placed skeletal meshes, by class and mesh asset (to tell characters
// from props); logged when their number changes, a few times per session.
void LogSkeletal() {
  if (!g_reflection || g_skeletal.size() == g_loggedSkeletal || g_skeletalLogs >= 4) return;
  g_loggedSkeletal = g_skeletal.size();
  ++g_skeletalLogs;
  VR_LOG_INFO("hide actors: %u placed skeletal mesh actor(s):", static_cast<unsigned>(g_skeletal.size()));
  int lines = 0;
  for (const Entry& e : g_skeletal) {
    if (++lines > 60) {
      VR_LOG_INFO("  ...");
      break;
    }
    uintptr_t component = Raw<uintptr_t>(e.object + g_skeletalActorComponent), mesh = 0;
    if (component && g_componentMesh > 0) mesh = Raw<uintptr_t>(component + static_cast<uint32_t>(g_componentMesh));
    VR_LOG_INFO("  %s: %s", g_reflection->FullName(e.object).c_str(),
                mesh ? g_reflection->FullName(mesh).c_str() : "(no mesh)");
  }
}

void Scan(uintptr_t playerPawn) {
  g_player.clear();
  g_npcs.clear();
  g_skeletal.clear();
  uintptr_t data = 0;
  int32_t num = 0;
  if (!vrcore::mem::Read(g_objectsArray, data) || !vrcore::mem::Read(g_objectsArray + 4, num) || !data) return;
  if (!vrcore::mem::IsReadable(reinterpret_cast<const void*>(data), static_cast<size_t>(num) * 4)) return;
  for (int32_t i = 0; i < num; ++i) {
    const uintptr_t object = Raw<uintptr_t>(data + 4u * i);
    if (!object || !IsA(object, g_actorClass)) continue;
    const Entry e{object, Raw<int32_t>(object + g_objIndex)};
    if (object == playerPawn || (playerPawn && (Raw<uintptr_t>(object + g_actorBase) == playerPawn ||
                                                Raw<uintptr_t>(object + g_actorOwner) == playerPawn))) {
      g_player.push_back(e);
    } else if (IsA(object, g_pawnClass) || (g_crowdClass && IsA(object, g_crowdClass))) {
      g_npcs.push_back(e);
    } else if (g_skeletalActorClass && IsA(object, g_skeletalActorClass)) {
      g_skeletal.push_back(e);
    }
  }
  // What's attached to the NPCs (heads, hair, props as separate actors).
  std::unordered_set<uintptr_t> npcs;
  for (const Entry& e : g_npcs) npcs.insert(e.object);
  const size_t pawns = g_npcs.size();
  for (int32_t i = 0; i < num && !npcs.empty(); ++i) {
    const uintptr_t object = Raw<uintptr_t>(data + 4u * i);
    if (!object || npcs.count(object) || object == playerPawn || !IsA(object, g_actorClass)) continue;
    if (npcs.count(Raw<uintptr_t>(object + g_actorBase)) || npcs.count(Raw<uintptr_t>(object + g_actorOwner))) {
      g_npcs.push_back({object, Raw<int32_t>(object + g_objIndex)});
    }
  }
  if (!g_loggedScan) {
    g_loggedScan = true;
    VR_LOG_INFO("hide actors: %u player actor(s), %u NPC pawn(s)/crowd agent(s) + %u attached",
                static_cast<unsigned>(g_player.size()), static_cast<unsigned>(pawns),
                static_cast<unsigned>(g_npcs.size() - pawns));
  }
  LogSkeletal();
}

}  // namespace

void Configure(const vrcore::Config& config) {
  g_hidePlayer = config.GetBool("Visibility", "HidePlayer", false);
  g_hideNpcs = config.GetBool("Visibility", "HideNpcs", false);
  g_hideSkeletal = config.GetBool("Visibility", "HideSkeletalMeshes", false);
}

void Save(const vrcore::Config& config) {
  config.SetBool("Visibility", "HidePlayer", g_hidePlayer);
  config.SetBool("Visibility", "HideNpcs", g_hideNpcs);
  config.SetBool("Visibility", "HideSkeletalMeshes", g_hideSkeletal);
}

void Init(const vrcore::ue3::Reflection& reflection) {
  if (g_ready || !reflection.Valid() || !reflection.TypesValid()) return;
  const auto& l = reflection.GetLayout();
  g_objectsArray = l.objectsArray;
  g_objIndex = l.objIndex;
  g_objClass = l.objClass;
  g_structSuper = l.structSuper;
  g_pawnClass = reflection.FindObject("Class Engine.Pawn");
  g_actorClass = reflection.FindObject("Class Engine.Actor");
  g_crowdClass = reflection.FindObject("Class Engine.CrowdAgentBase");
  g_skeletalActorClass = reflection.FindObject("Class Engine.SkeletalMeshActor");
  g_componentMesh = reflection.PropertyOffset("Class Engine.SkeletalMeshComponent", "SkeletalMesh");
  const int32_t component = reflection.PropertyOffset("Class Engine.SkeletalMeshActor", "SkeletalMeshComponent");
  if (component > 0) g_skeletalActorComponent = static_cast<uint32_t>(component);
  g_reflection = &reflection;
  const auto offset = [&](const char* cls, const char* prop, uint32_t& out) {
    const int32_t o = reflection.PropertyOffset(cls, prop);
    if (o > 0) out = static_cast<uint32_t>(o);
    return o > 0;
  };
  const bool ok = g_pawnClass && g_actorClass && g_structSuper &&
                  offset("Class Engine.Controller", "Pawn", g_controllerPawn) &&
                  offset("Class Engine.PlayerController", "HiddenActors", g_hiddenActors) &&
                  offset("Class Engine.Actor", "Owner", g_actorOwner) && offset("Class Engine.Actor", "Base", g_actorBase);
  if (!ok) {
    VR_LOG_WARN("hide actors: classes/properties not found; hiding the player/NPCs unavailable");
    return;
  }
  g_buffer.reserve(kCapacity);
  g_ready = true;
  VR_LOG_INFO("hide actors: ready (Controller.Pawn +0x%x, PlayerController.HiddenActors +0x%x)", g_controllerPawn,
              g_hiddenActors);
}

bool Available() { return g_ready; }
void SetHidePlayer(bool hide) { g_hidePlayer = hide; }
void SetHideNpcs(bool hide) { g_hideNpcs = hide; }
bool HidePlayer() { return g_hidePlayer; }
bool HideNpcs() { return g_hideNpcs; }
void SetHideSkeletalMeshes(bool hide) { g_hideSkeletal = hide; }
bool HideSkeletalMeshes() { return g_hideSkeletal; }

void Update(void* localPlayer) {
  if (!g_ready || (!g_hidePlayer && !g_hideNpcs && !g_hideSkeletal)) return;
  const uintptr_t pawn = PlayerPawn(localPlayer);
  const uint64_t now = GetTickCount64();
  if (now < g_nextScanMs && pawn == g_scannedPawn) return;
  g_nextScanMs = now + 1000;
  g_scannedPawn = pawn;
  Scan(pawn);
}

Scope::Scope(void* localPlayer) {
  if (!g_ready || (!g_hidePlayer && !g_hideNpcs && !g_hideSkeletal) || !localPlayer) return;
  uintptr_t controller = 0;
  if (!vrcore::mem::Read(reinterpret_cast<uintptr_t>(localPlayer) + kLocalPlayerActor, controller) || !controller) {
    return;
  }
  const uintptr_t array = controller + g_hiddenActors;
  if (!vrcore::mem::Read(array, saved_)) return;
  const auto gameData = static_cast<uintptr_t>(saved_[0]);
  const auto gameNum = static_cast<int32_t>(saved_[1]);
  g_buffer.clear();
  for (int32_t i = 0; i < gameNum && gameData && g_buffer.size() < kCapacity; ++i) {
    g_buffer.push_back(Raw<uint32_t>(gameData + 4u * i));
  }
  const auto add = [](const std::vector<Entry>& list) {
    for (const Entry& e : list) {
      if (g_buffer.size() < kCapacity && Alive(e)) g_buffer.push_back(static_cast<uint32_t>(e.object));
    }
  };
  if (g_hidePlayer) add(g_player);
  if (g_hideNpcs) add(g_npcs);
  if (g_hideSkeletal) add(g_skeletal);
  if (static_cast<int32_t>(g_buffer.size()) == gameNum) return;  // nothing of ours
  auto* header = reinterpret_cast<uint32_t*>(array);
  header[0] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_buffer.data()));
  header[1] = static_cast<uint32_t>(g_buffer.size());
  header[2] = static_cast<uint32_t>(kCapacity);
  array_ = array;
}

Scope::~Scope() {
  if (!array_) return;
  auto* header = reinterpret_cast<uint32_t*>(array_);
  header[0] = saved_[0];
  header[1] = saved_[1];
  header[2] = saved_[2];
}

}  // namespace rm::hide_actors
