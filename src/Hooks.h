#pragma once

// vtable hooks only (no function-start detours): PlayerCharacter vtbl[0] 0xAD Update -- the session clock and steps;
// 0x7D UpdateAnimation -- clip echo before the graph update, held-back events after it; vtbl[3] slot 1
// NotifyAnimationGraph -- events sent to the player's graph. Sinks: PoisonedWeapon::Event, the player's graph events.
// One call-site hook: the behavior project link in BShkbAnimationGraph::InitImpl -- the graph repair (request 29).

namespace Hooks
{
	void InstallEarly();  // SKSEPluginLoad: before any behavior project is loaded
	void Install();       // kDataLoaded
	void OnGameLoaded();  // graphs are rebuilt with the 3D: subscribe again
}
