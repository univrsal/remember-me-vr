#pragma once

#include "vrcore/config.h"
#include "vrcore/ue3.h"

namespace rm::hide_actors {

// Hides the player and/or the other pawns (NPCs, enemies) from the rendered
// views, through UE3's per-player hidden actors: ULocalPlayer::CalcSceneView
// adds every component of each actor in PlayerController.HiddenActors (+0x604)
// to the view's hidden primitives (components with
// bIgnoreHiddenActorsMembership excepted). The game's own array isn't
// touched: during our CalcSceneView calls the controller's TArray header is
// swapped for a copy holding the game's entries plus ours (large capacity, so
// the engine's in-place edits, e.g. dropping null entries, never reallocate),
// and put back afterwards. Gameplay, collision and audio are unaffected;
// shadows may remain.
//
// The player: PlayerController.Pawn plus actors based on or owned by it. NPCs:
// every other Pawn (rescanned from GObjects about once a second).

void Configure(const vrcore::Config& config);
void Save(const vrcore::Config& config);
// Once reflection is mapped (property offsets, the Pawn class).
void Init(const vrcore::ue3::Reflection& reflection);
bool Available();

void SetHidePlayer(bool hide);
void SetHideNpcs(bool hide);
bool HidePlayer();
bool HideNpcs();

// Game thread, once per frame (main CalcSceneView): refreshes the actor list.
void Update(void* localPlayer);

// Game thread: swaps the hidden actors in for the lifetime of the scope.
class Scope {
 public:
  explicit Scope(void* localPlayer);
  ~Scope();
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  uintptr_t array_ = 0;  // controller + HiddenActors, or 0 if nothing swapped
  uint32_t saved_[3] = {};
};

}  // namespace rm::hide_actors
