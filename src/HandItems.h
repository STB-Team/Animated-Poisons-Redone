#pragma once

#include "Placement.h"

// The bottle, the poison FX and the poisoned arrow / bolt in the hand: the plugin's own copies of the models on the
// player's skeleton (3P, and 1P for the *1P items). Positions come from our JSON / built-in values (Placement).
// Attached like any equipped extra: model clone, addon nodes / particles, the actor's fade-node shaders, AttachChild to
// the hand node with the item's local transform (extrinsic Euler, degrees), shadow-scene registration, the projectile's
// flight trail and the collision removed. Shown / hidden with the app-culled flag, removed with the engine's object
// cleanup + DetachChild. Main thread only.

namespace HandItems
{
	// create (hidden) or replace the item a_name: a_model relative to Data/meshes, a_node of the 3P skeleton -- or of the
	// 1P one (a_firstPerson)
	bool Create(RE::Actor* a_actor, const char* a_name, const std::string& a_model, const char* a_node, const Placement::Transform& a_t,
		bool a_projectile, bool a_firstPerson = false);

	void Show(const char* a_name, bool a_on);
	void RemoveAll();  // end of a session, game load

	// a chained repeat -- the items stay as they are for a_frames more frames, then go (Tick); the next session's
	// own items are shown meanwhile: a short overlap instead of 1-2 frames without a bottle / arrow
	void RetireAll(int a_frames);
	void Tick();  // every frame
}
