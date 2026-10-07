#pragma once

// The log tells what is wrong on someone else's machine: game, SKSE / Address Library, dependencies and their versions,
// the mod's files, the mod's Nemesis patch in the graphs, other mods on our hooks. Problems are logged always (error /
// warning); the full report (every SKSE plugin, every file checked) only with bDebugLog.

namespace Diagnostics
{
	void OnLoad(const SKSE::LoadInterface* a_skse);  // SKSEPlugin_Load: game, SKSE, Address Library
	void OnDataLoaded();                              // dependencies, plugins, files, related mods
	void OnGraph(RE::BShkbAnimationGraph* a_graph);   // a player graph: OMA / the mod's Nemesis patch in it?

	std::string ModuleOf(std::uintptr_t a_address);       // "SkyrimSE.exe+0x6C7F40" / "SomeMod.dll+0x1234"
	bool        FileExists(const std::string& a_dataPath);  // loose or in a BSA, path relative to Data
	bool        OarLoaded();                                // OpenAnimationReplacer.dll is loaded
}
