#pragma once

// The log tells what is wrong on someone else's machine: game, SKSE / Address Library, dependencies and their versions,
// the mod's files, the mod's Nemesis patch in the graphs, other mods on our hooks. Problems are logged always (error /
// warning); the full report (every SKSE plugin, every file checked) only with bDebugLog.

namespace Diagnostics
{
	void OnLoad(const SKSE::LoadInterface* a_skse);  // SKSEPlugin_Load: game, SKSE, Address Library
	void OnDataLoaded();                              // dependencies, plugins, files, related mods
	void OnGraph(RE::BShkbAnimationGraph* a_graph);   // a player graph: OMA / the mod's Nemesis patch in it?

	// a behavior project's template right after it is linked (any thread, before any actor clones it): transitions into a
	// state their machine does not have are disabled -- else the first event crashes the game (request 29)
	void RepairGraph(RE::hkbBehaviorGraph* a_root, const char* a_project);
	bool OmaStateMissing();  // a transition to OMA's pose state 1233 in Master_Behavior was repaired: OMA is not in the behavior

	std::string ModuleOf(std::uintptr_t a_address);       // "SkyrimSE.exe+0x6C7F40" / "SomeMod.dll+0x1234"
	bool        FileExists(const std::string& a_dataPath);  // loose or in a BSA, path relative to Data
	bool        OarLoaded();                                // OpenAnimationReplacer.dll is loaded
}
