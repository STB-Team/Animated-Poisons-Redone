#include "Diagnostics.h"

#include "Settings.h"

#include <chrono>
#include <optional>
#include <set>
#include <unordered_set>

#include <Psapi.h>
#pragma comment(lib, "Psapi.lib")

namespace Diagnostics
{
	namespace
	{
		bool Full() { return Settings::Get().debugLog; }

		std::string Narrow(const std::wstring& a_text)
		{
			return SKSE::stl::utf16_to_utf8(a_text).value_or("?");
		}

		// FileVersion of a DLL / exe, "" if it has none
		std::string FileVersion(const std::wstring& a_path)
		{
			DWORD      handle = 0;
			const auto size = GetFileVersionInfoSizeW(a_path.c_str(), &handle);
			if (!size) {
				return {};
			}
			std::vector<std::byte> data(size);
			VS_FIXEDFILEINFO*      info = nullptr;
			UINT                   len = 0;
			if (!GetFileVersionInfoW(a_path.c_str(), 0, size, data.data()) ||
				!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info) {
				return {};
			}
			return fmt::format("{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS),
				LOWORD(info->dwFileVersionLS));
		}

		struct Module
		{
			std::wstring path;
			std::string  name;  // file name, as loaded
			std::string  lower;
		};

		std::vector<Module> LoadedModules()
		{
			std::vector<Module> out;
			HMODULE             modules[1024];
			DWORD               needed = 0;
			if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
				return out;
			}
			for (DWORD i = 0; i < std::min<DWORD>(needed / sizeof(HMODULE), 1024); ++i) {
				wchar_t path[MAX_PATH]{};
				if (!GetModuleFileNameW(modules[i], path, MAX_PATH)) {
					continue;
				}
				Module m{ path, Narrow(std::filesystem::path(path).filename().wstring()), {} };
				m.lower = m.name;
				std::ranges::transform(m.lower, m.lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				out.push_back(std::move(m));
			}
			return out;
		}

		const Module* FindModule(const std::vector<Module>& a_modules, std::string_view a_lowerPart)
		{
			for (auto& m : a_modules) {
				if (m.lower.find(a_lowerPart) != std::string::npos) {
					return &m;
				}
			}
			return nullptr;
		}

		// what we know about mods that touch the same things (checked against their DLLs / sources, 2026-10-06)
		struct Known
		{
			const char* dll;  // lower-case part of the DLL name
			const char* name;
			const char* note;
		};
		constexpr Known kKnownDlls[] = {
			{ "improvedcamerase", "Improved Camera SE", "1P with the body: the bottle is put on the 3P body as well" },
			{ "smoothcam", "SmoothCam", "no overlap" },
			{ "skyrimsoulsre", "Skyrim Souls RE", "unpaused menus: the pose plays in the inventory unless bPlayInMenus = false" },
			{ "ultimatecombat", "Ultimate Combat", "takes over the player's NotifyAnimationGraph: hooked again on top of it" },
			{ "insertattackdata", "Ultimate Combat (InsertAttackData)", "no overlap" },
			{ "bfco", "BFCO", "its attacks end the pose (BFCO events, IsAttacking)" },
			{ "mco.dll", "MCO Universal Support", "no overlap" },
			{ "tk_dodge_re", "TK Dodge RE", "its dodges end the pose (TKDodge* events, bIsDodging / bInIframe)" },
			{ "valhallacombat", "Valhalla Combat", "shares PlayerCharacter::Update (chained)" },
			{ "eldencounter", "Elden Counter", "shares PlayerCharacter::Update (chained)" },
			{ "scar.dll", "SCAR", "no overlap" },
			{ "behaviordatainjector", "Behavior Data Injector", "no overlap" },
			{ "dynamicanimationcasting", "Dynamic Animation Casting", "no overlap" },
			{ "netscriptframework", "NetScriptFramework (IFPV?)", "IFPV switches the 1P/3P graph on draw: a pose may end right after drawing" },
			{ "precision", "Precision", "not checked" },
			{ "dtry_keyutil", "Stances / KeyUtils", "not checked" },
		};
		constexpr std::pair<const char*, const char*> kKnownPlugins[] = {
			{ "Ultimate Dodge Mod.esp", "TUDM: its rolls end the pose (clip events RollTrigger / SidestepTrigger, RollStart)" },
			{ "SCSI-ACTbfco-Main.esp", "BFCO data" },
			{ "UltimateCombat.esp", "Ultimate Combat" },
			{ "ImmersiveInteractions.esp", "works alongside: its poison animation (AR_Poison) is turned off by the DLL, the rest stays" },
			{ "Animated Poisons.esp", "INCOMPATIBLE: two poison animations -- disable it" },
		};

		void CheckFile(const char* a_what, const std::string& a_path, bool a_required, bool a_looseOnly = false)
		{
			const bool ok = a_looseOnly ? std::filesystem::exists("Data/" + a_path) : FileExists(a_path);
			if (!ok && a_required) {
				logger::error("file missing: {} ({})", a_path, a_what);
			} else if (Full()) {
				logger::info("  file {}: {} ({})", ok ? "ok" : "missing", a_path, a_what);
			}
		}

		std::set<std::string> g_graphsChecked;
		bool                  g_oarLoaded{ false };

		// --- transitions into a state that is not in their machine (request 29, 4.7.11) ----------------------------
		// hkbStateMachine::findBestTransitions (SE 58719 / AE 59380) reads states[getStateIndex(toStateId)]->enable for
		// every transition of the array on every event that reaches the machine, before the event, flags and condition:
		// getStateIndex (SE 58711) gives -1 for an unknown id, nothing checks it -> states[-1] = the heap header -> crash on
		// the first event (a player's or an NPC's, any thread). The mod's Nemesis patch run without OMA's does exactly that
		// (APR_PoisonStart -> OMA's state 1233). Repaired in the project's template right after it is linked, before any
		// actor clones it: the transition is pointed to an existing state and gets FLAG_DISABLED (checked only after the
		// enable read). Clones share the template's transition arrays (the clone ctor SE 58675 add-refs wildcardTransitions;
		// StateInfo objects are shared or copied with their transitions), so every actor of the project gets the fix.
		// Event ids in the template are the file's own (linking only sets the graph's eventIDMap).
		// Layout (Havok 2010, SE database types, the same on AE): hkbNode vtbl 8 getMaxNumChildren(flags), 9
		// getChildren(flags, ChildrenInfo*) -- ChildrenInfo {hkArray<ChildInfo>*, flags}, ChildInfo 0x10 with the node at +0
		// (as BShkbUtils::GraphTraverser::Next, SE 62944); StateInfo +0x50 transitions, +0x60 name, +0x68 stateId;
		// TransitionInfoArray +0x10 hkArray<TransitionInfo>; TransitionInfo 0x48: +0x30 eventId, +0x34 toStateId, +0x42 flags

		constexpr std::int32_t  kOmaPoseState = 1233;  // OMA's GPMAOffsetState in Master_Behavior, the target of APR_PoisonStart
		constexpr std::uint16_t kFlagDisabled = 0x20;  // hkbStateMachine::TransitionInfo::FLAG_DISABLED

		std::atomic_bool g_omaStateMissing{ false };

		struct ChildInfo
		{
			RE::hkbNode*  node;
			std::uint64_t unk08;
		};
		static_assert(sizeof(ChildInfo) == 0x10);

		struct ChildInfoArray
		{
			ChildInfo*   data;
			std::int32_t size;
			std::int32_t capacityAndFlags;
		};

		struct ChildrenInfo
		{
			ChildInfoArray* childInfos;
			std::uint32_t   flags;
			std::uint32_t   pad0C;
		};

		struct TransitionInfo
		{
			std::uint8_t  unk00[0x30];
			std::int32_t  eventId;           // 30
			std::int32_t  toStateId;         // 34
			std::int32_t  fromNestedStateId; // 38
			std::int32_t  toNestedStateId;   // 3C
			std::int16_t  priority;          // 40
			std::uint16_t flags;             // 42
			std::uint32_t pad44;             // 44
		};
		static_assert(sizeof(TransitionInfo) == 0x48);

		struct TransitionArray
		{
			std::uint8_t    unk00[0x10];
			TransitionInfo* data;  // 10
			std::int32_t    size;  // 18
		};

		template <class T>
		T& At(const void* a_base, std::ptrdiff_t a_offset)
		{
			return *reinterpret_cast<T*>(reinterpret_cast<std::uintptr_t>(a_base) + a_offset);
		}

		const char* Str(const RE::hkStringPtr& a_string) { return a_string.c_str() ? a_string.c_str() : "?"; }

		// every child of the node (flags 0: not only the active ones, referenced behaviors too), into our buffer of
		// getMaxNumChildren entries; if the node gives more, the engine grows the array on its heap -- freed here
		void Children(RE::hkbNode* a_node, std::vector<RE::hkbNode*>& a_out)
		{
			const auto vtbl = *reinterpret_cast<const std::uintptr_t* const*>(a_node);
			const auto max = reinterpret_cast<std::int32_t (*)(RE::hkbNode*, std::int32_t)>(vtbl[8])(a_node, 0);
			std::vector<ChildInfo> buffer(static_cast<std::size_t>(std::max(max, 0)));
			ChildInfoArray         array{ buffer.data(), 0, static_cast<std::int32_t>(0x80000000u | static_cast<std::uint32_t>(buffer.size())) };
			ChildrenInfo           info{ &array, 0, 0 };
			reinterpret_cast<void (*)(RE::hkbNode*, std::int32_t, ChildrenInfo*)>(vtbl[9])(a_node, 0, &info);
			for (std::int32_t i = 0; i < array.size; ++i) {
				if (array.data[i].node) {
					a_out.push_back(array.data[i].node);
				}
			}
			if (array.data && array.data != buffer.data() && array.capacityAndFlags >= 0) {
				RE::hkContainerHeapAllocator::GetSingleton()->BufFree(array.data, (array.capacityAndFlags & 0x3FFFFFFF) * static_cast<std::int32_t>(sizeof(ChildInfo)));
			}
		}

		const char* EventName(RE::hkbBehaviorGraph* a_graph, std::int32_t a_id)
		{
			const auto data = a_graph ? a_graph->data.get() : nullptr;
			const auto strings = data ? data->stringData.get() : nullptr;
			if (a_id < 0) {
				return "(none)";
			}
			return strings && a_id < static_cast<std::int32_t>(strings->eventNames.size()) ? Str(strings->eventNames[a_id]) : "?";
		}

		int RepairMachine(RE::hkbStateMachine* a_machine, RE::hkbBehaviorGraph* a_graph, const char* a_project)
		{
			const auto& states = a_machine->states;
			const auto  has = [&](std::int32_t a_id) {
                for (const auto state : states) {
                    if (state && At<std::int32_t>(state, 0x68) == a_id) {
                        return true;
                    }
                }
                return false;
			};
			// an existing state to point a broken transition to: the start state, else the first one
			std::optional<std::int32_t> target;
			if (has(a_machine->startStateID)) {
				target = a_machine->startStateID;
			} else if (!states.empty() && states[0]) {
				target = At<std::int32_t>(states[0], 0x68);
			}
			const char* machine = Str(a_machine->name);
			int         repaired = 0;
			const auto  repair = [&](void* a_array, const char* a_from) {
                if (!a_array) {
                    return;
                }
                auto& transitions = *static_cast<TransitionArray*>(a_array);
                for (std::int32_t i = 0; i < transitions.size; ++i) {
                    auto&      t = transitions.data[i];
                    const auto missing = t.toStateId;
                    if (has(missing)) {
                        continue;
                    }
                    if (!target) {
                        logger::error("behavior {}: state machine '{}' has no states, its {} transition {} to state {} cannot be repaired",
                            a_project, machine, a_from, i, missing);
                        continue;
                    }
                    t.toStateId = *target;
                    t.flags |= kFlagDisabled;
                    ++repaired;
                    const bool oma = missing == kOmaPoseState && _stricmp(machine, "Master_Behavior") == 0;
                    logger::error("behavior {} ({}): state machine '{}', {} transition {} on event '{}' leads to state {}, which is not "
                                  "in the machine -- disabled (pointed to state {}), else the first event to it crashes the game{}",
                        a_project, Str(a_graph ? a_graph->name : a_machine->name), machine, a_from, i, EventName(a_graph, t.eventId), missing,
                        *target, oma ? " (OMA's pose state: Offset Movement Animation is not in the behavior)" : "");
                    if (oma) {
                        g_omaStateMissing = true;
                    }
                }
			};
			repair(a_machine->wildcardTransitions.get(), "wildcard");
			for (const auto state : states) {
				if (state) {
					repair(At<void*>(state, 0x50), fmt::format("state '{}'", Str(At<RE::hkStringPtr>(state, 0x60))).c_str());
				}
			}
			return repaired;
		}
	}

	bool OmaStateMissing() { return g_omaStateMissing; }

	void RepairGraph(RE::hkbBehaviorGraph* a_root, const char* a_project)
	{
		if (!a_root) {
			return;
		}
		static const auto machineVtbl = RE::VTABLE_hkbStateMachine[0].address();
		static const auto graphVtbl = RE::VTABLE_hkbBehaviorGraph[0].address();
		const auto        project = a_project && *a_project ? a_project : "?";
		const auto        start = std::chrono::steady_clock::now();

		std::unordered_set<RE::hkbNode*>                            seen{ a_root };
		std::vector<std::pair<RE::hkbNode*, RE::hkbBehaviorGraph*>> stack{ { a_root, a_root } };
		std::vector<RE::hkbNode*>                                   children;
		int                                                         machines = 0;
		int                                                         repaired = 0;
		while (!stack.empty()) {
			auto [node, graph] = stack.back();
			stack.pop_back();
			const auto vtbl = *reinterpret_cast<const std::uintptr_t*>(node);
			if (vtbl == graphVtbl) {
				graph = static_cast<RE::hkbBehaviorGraph*>(node);  // a referenced behavior file: its own event names
			} else if (vtbl == machineVtbl) {
				++machines;
				repaired += RepairMachine(static_cast<RE::hkbStateMachine*>(node), graph, project);
			}
			children.clear();
			Children(node, children);
			for (const auto child : children) {
				if (seen.insert(child).second) {
					stack.emplace_back(child, graph);
				}
			}
		}
		if (repaired > 0 || Full()) {
			const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			logger::info("behavior {} ({}) checked at load: {} nodes, {} state machines, {} transitions into missing states disabled ({:.1f} ms)",
				project, Str(a_root->name), seen.size(), machines, repaired, ms);
		}
	}

	std::string ModuleOf(std::uintptr_t a_address)
	{
		HMODULE module = nullptr;
		if (!a_address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
							  reinterpret_cast<LPCWSTR>(a_address), &module) ||
			!module) {
			return fmt::format("0x{:X} (no module)", a_address);
		}
		wchar_t path[MAX_PATH]{};
		GetModuleFileNameW(module, path, MAX_PATH);
		return fmt::format("{}+0x{:X}", Narrow(std::filesystem::path(path).filename().wstring()), a_address - reinterpret_cast<std::uintptr_t>(module));
	}

	bool OarLoaded() { return g_oarLoaded; }

	bool FileExists(const std::string& a_dataPath)
	{
		RE::BSResourceNiBinaryStream stream(a_dataPath);
		return stream.good();
	}

	void OnLoad(const SKSE::LoadInterface* a_skse)
	{
		const auto game = REL::Module::get().version();
		const auto skse = REL::Version::unpack(a_skse->SKSEVersion());
		const auto addrlib = REL::Module::IsAE() ? fmt::format("Data/SKSE/Plugins/versionlib-{}.bin", game.string()) :
		                                           fmt::format("Data/SKSE/Plugins/version-{}.bin", game.string());
		std::error_code ec;
		const bool      haveLib = std::filesystem::exists(addrlib, ec);
		logger::info("environment: game {} ({}), SKSE {}, Address Library {} {}", game.string("."), REL::Module::IsAE() ? "AE" : "SE",
			skse.string("."), addrlib, haveLib ? "found" : "NOT FOUND");
		if (!haveLib) {
			logger::error("Address Library for this game version is missing: {} -- install 'Address Library for SKSE Plugins'", addrlib);
		}
	}

	void OnDataLoaded()
	{
		const auto modules = LoadedModules();

		// required DLL: OAR replaces OMA's placeholder clip with the mod's. OAR itself is OpenAnimationReplacer.dll -- its
		// add-ons (OpenAnimationReplacer-Math.dll ...) are not it
		const Module* oar = nullptr;
		for (auto& m : modules) {
			if (m.lower == "openanimationreplacer.dll") {
				oar = &m;
			} else if (m.lower.find("openanimationreplacer") != std::string::npos) {
				logger::info("OAR add-on: {} {} (needs Open Animation Replacer itself)", m.name, FileVersion(m.path));
			}
		}
		logger::info("dependencies: Open Animation Replacer {}", oar ? oar->name + " " + FileVersion(oar->path) : std::string("NOT LOADED"));
		g_oarLoaded = oar != nullptr;
		if (!oar) {
			logger::error("Open Animation Replacer is not loaded: the pose would be OMA's empty placeholder that ends at 0.2 s. "
						  "Installed? Built for this game version? (skse64.log tells why a DLL was not loaded)");
		} else if (const auto v = FileVersion(oar->path); !v.empty() && (std::stoi(v) < 3 || (std::stoi(v) == 3 && std::stoi(v.substr(v.find('.') + 1)) < 2))) {
			logger::warn("Open Animation Replacer {} is older than 3.2: the speed setting (fAnimSpeed) is ignored", v);
		}

		// every SKSE / NetScriptFramework plugin with its version -- conflicts are found by the name
		if (Full()) {
			logger::info("loaded plugins:");
			for (auto& m : modules) {
				std::wstring lowerPath = m.path;
				std::ranges::transform(lowerPath, lowerPath.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
				if (lowerPath.find(L"\\skse\\plugins\\") != std::wstring::npos || m.lower.find("netscriptframework") != std::string::npos) {
					logger::info("  {} {}", m.name, FileVersion(m.path));
				}
			}
		}
		for (auto& k : kKnownDlls) {
			if (const auto m = FindModule(modules, k.dll)) {
				logger::info("related mod: {} ({} {}) -- {}", k.name, m->name, FileVersion(m->path), k.note);
			}
		}
		if (std::filesystem::exists("Data/NetScriptFramework/Plugins/IFPV.dll")) {
			logger::info("related mod: IFPV (NetScriptFramework/Plugins/IFPV.dll) -- 1P/3P graph switches on draw: a pose may end right after drawing");
		}

		// known plugins of other mods
		const auto data = RE::TESDataHandler::GetSingleton();
		if (data) {
			for (auto& [file, note] : kKnownPlugins) {
				if (data->LookupLoadedModByName(file) || data->LookupLoadedLightModByName(file)) {
					logger::info("related plugin: {} -- {}", file, note);
				}
			}
		}

		// the mod's files. OAR reads its folders loose (not from BSA); OMA's clips may be in a BSA
		if (Full()) {
			logger::info("files:");
		}
		CheckFile("OMA, 3P placeholder clip", "meshes\\actors\\character\\animations\\GPMAOffsetAnimation.hkx", true);
		CheckFile("OMA, 1P placeholder clip", "meshes\\actors\\character\\_1stperson\\animations\\GPMAOffsetAnimation_IC_.hkx", true);
		for (const auto* root : { "meshes/actors/character/animations", "meshes/actors/character/_1stperson/animations" }) {
			const std::string base = std::string(root) + "/OpenAnimationReplacer/AnimatedPoisonsRedone";
			const bool        first = std::string_view(root).find("_1stperson") != std::string_view::npos;
			const char*       clip = first ? "GPMAOffsetAnimation_IC_.hkx" : "GPMAOffsetAnimation.hkx";
			CheckFile(first ? "OAR mod, 1P" : "OAR mod, 3P", base + "/config.json", true, true);
			for (const auto* sub : { "Poison", "PoisonArrow" }) {
				CheckFile("OAR sub-mod", base + "/" + sub + "/config.json", true, true);
				CheckFile("OAR clip", base + "/" + sub + "/" + clip, true, true);
			}
		}
		CheckFile("INI", "SKSE/Plugins/AnimatedPoisonsRedone.ini", false, true);
	}

	void OnGraph(RE::BShkbAnimationGraph* a_graph)
	{
		if (!a_graph || !a_graph->behaviorGraph) {
			return;
		}
		const std::string project = a_graph->projectName.c_str() ? a_graph->projectName.c_str() : "?";
		if (!g_graphsChecked.insert(project).second) {
			return;
		}
		const auto data = a_graph->behaviorGraph->data.get();
		const auto strings = data ? data->stringData.get() : nullptr;
		if (!strings) {
			logger::warn("graph {}: no string data, events not checked", project);
			return;
		}
		auto has = [](const RE::hkArray<RE::hkStringPtr>& a_list, const char* a_name) {
			for (auto& s : a_list) {
				if (s.c_str() && _stricmp(s.c_str(), a_name) == 0) {
					return true;
				}
			}
			return false;
		};
		// OMA's event and variable, and its pose state: a transition into the missing state 1233 was repaired at load (4.7.11)
		const bool oma = has(strings->eventNames, "OffsetGPMA") && has(strings->variableNames, "bOffsetGPMA") && !g_omaStateMissing;
		const bool start = has(strings->eventNames, "APR_PoisonStart");
		const bool stop = has(strings->eventNames, "APR_PoisonStop");
		const bool vars = has(strings->variableNames, "APR_PoisonAnim") && has(strings->variableNames, "APR_PoisonSpeed");  // v43
		logger::info("graph {}: {} events, {} variables; OMA {}, the mod's Nemesis patch {} (APR_PoisonStart), APR_PoisonStop {}, "
					 "APR_PoisonAnim / APR_PoisonSpeed {}",
			project, strings->eventNames.size(), strings->variableNames.size(), oma ? "yes" : "NO", start ? "yes" : "NO", stop ? "yes" : "no",
			vars ? "yes" : "NO");
		if (!oma) {
			logger::error("graph {}: Offset Movement Animation is not in the behavior (OffsetGPMA / bOffsetGPMA or its state 1233 missing) -- "
						  "OMA not installed or Nemesis / Pandora not run with it: no animation",
				project);
		} else if (!start || !vars) {
			logger::error("graph {}: the mod's Nemesis patch is not in the behavior ({} missing) -- run Nemesis / Pandora "
						  "with 'Animated Poisons Redone' ticked",
				project, !start ? "APR_PoisonStart" : "APR_PoisonAnim / APR_PoisonSpeed");
		}
	}
}
