#include "Hooks.h"

#include "Diagnostics.h"

#include "Poison.h"
#include "Settings.h"

namespace Hooks
{
	namespace
	{
		// --- PoisonedWeapon::Event: the player confirmed poisoning (SE 39407 sends it after PoisonObject + RemoveItem,
		//     PlayerCharacter::pendingPoison is cleared only after the send) ---------------------------------------

		// PlayerCharacter::pendingPoison: SE +0x968, AE 1.6.629+ +0x970, 1.7.104 +0x978 -- in 1.7 PlayerCharacter got
		// 8 more bytes around +0x2D8, which CommonLibSSE-NG does not know (its GetInfoRuntimeData() gives 0x970 there).
		// The offset is read from the game's own poison confirm callback (SE 39407 / AE 40482), the very function that
		// sends this event: it starts `40 53 48 83 EC xx 48 8B 05 <player> 48 83 B8 <disp32> 00` = cmp [player+disp], 0.
		// Same bytes on 1.5.97, 1.6.1170 and 1.7.104 (plugins/AnimatedPoisonsRedone/tools/check_ids.py). No match -> NG's offset.
		std::ptrdiff_t PendingPoisonOffset()
		{
			static const std::ptrdiff_t offset = [] {
				const auto code = reinterpret_cast<const std::uint8_t*>(REL::Relocation<std::uintptr_t>{ RELOCATION_ID(39407, 40482) }.address());
				if (code[0xD] == 0x48 && code[0xE] == 0x83 && code[0xF] == 0xB8 && code[0x14] == 0x00) {
					std::int32_t disp;
					std::memcpy(&disp, code + 0x10, sizeof(disp));
					if (disp >= 0x900 && disp <= 0xA00) {
						logger::info("pendingPoison at +0x{:X} (read from the game's poison callback)", disp);
						return static_cast<std::ptrdiff_t>(disp);
					}
				}
				const auto player = RE::PlayerCharacter::GetSingleton();
				const auto ng = reinterpret_cast<std::uintptr_t>(&player->GetInfoRuntimeData().pendingPoison) - reinterpret_cast<std::uintptr_t>(player);
				logger::warn("pendingPoison: unexpected code in the poison callback, using CommonLib's +0x{:X}", ng);
				return static_cast<std::ptrdiff_t>(ng);
			}();
			return offset;
		}

		RE::AlchemyItem* PendingPoison(RE::PlayerCharacter* a_player)
		{
			return *reinterpret_cast<RE::AlchemyItem**>(reinterpret_cast<std::uintptr_t>(a_player) + PendingPoisonOffset());
		}

		class PoisonSink : public RE::BSTEventSink<RE::PoisonedWeapon::Event>
		{
		public:
			static PoisonSink* Get()
			{
				static PoisonSink sink;
				return &sink;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::PoisonedWeapon::Event*, RE::BSTEventSource<RE::PoisonedWeapon::Event>*) override
			{
				if (const auto player = RE::PlayerCharacter::GetSingleton()) {
					Poison::OnPoisoned(PendingPoison(player));
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		RE::BSTEventSource<RE::PoisonedWeapon::Event>* PoisonedWeaponSource()
		{
			using func_t = RE::BSTEventSource<RE::PoisonedWeapon::Event>* (*)();
			REL::Relocation<func_t> func{ RELOCATION_ID(40058, 41069) };  // function-local static source (SE 0x6C7F40)
			return func();
		}

		// --- graph events of the player's active graph (1P or 3P), only while a session listens (or bDebugLog) ----------

		class GraphSink : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
		{
		public:
			static GraphSink* Get()
			{
				static GraphSink sink;
				return &sink;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_event, RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_source) override
			{
				if (!a_event || !a_source || !Poison::Listening()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto player = RE::PlayerCharacter::GetSingleton();
				RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
				if (!player || !player->GetAnimationGraphManager(manager) || !manager || manager->GetRuntimeData().activeGraph >= manager->graphs.size()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto active = manager->graphs[manager->GetRuntimeData().activeGraph].get();
				if (static_cast<RE::BSTEventSource<RE::BSAnimationGraphEvent>*>(active) == a_source) {
					Poison::OnGraphEvent(a_event->tag, a_event->payload);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		std::mutex                            g_sinkLock;
		RE::BSAnimationGraphManager*          g_sinkManager = nullptr;
		std::vector<RE::BShkbAnimationGraph*> g_sinkGraphs;

		void EnsureGraphSinks(RE::PlayerCharacter* a_player)
		{
			RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
			if (!a_player->GetAnimationGraphManager(manager) || !manager) {
				return;
			}
			std::lock_guard lock(g_sinkLock);
			if (manager.get() == g_sinkManager && manager->graphs.size() == g_sinkGraphs.size()) {
				return;  // every frame: same graphs, all subscribed
			}
			if (manager.get() != g_sinkManager) {
				g_sinkManager = manager.get();
				g_sinkGraphs.clear();
			}
			for (auto& graph : manager->graphs) {
				if (graph && std::find(g_sinkGraphs.begin(), g_sinkGraphs.end(), graph.get()) == g_sinkGraphs.end()) {
					static_cast<RE::BSTEventSource<RE::BSAnimationGraphEvent>*>(graph.get())->AddEventSink(GraphSink::Get());
					g_sinkGraphs.push_back(graph.get());
					logger::info("graph sink: {}", graph->projectName.c_str());
					Diagnostics::OnGraph(graph.get());
				}
			}
		}

		// --- IAnimationGraphManagerHolder::NotifyAnimationGraph of the player (vtbl[3] slot 1; SE 37020 / AE 38048): every
		//     animation event sent TO the player's graph by anyone -- a DLL, Papyrus Debug.SendAnimationEvent, the engine
		//     (its actions and idles). An action during the pose is held back (Poison::DeferPoseExit); everything else
		//     goes to the original first and is only queued for the session's log / checks ------------------------------

		struct PlayerNotify
		{
			static bool thunk(RE::IAnimationGraphManagerHolder* a_this, const RE::BSFixedString& a_event)
			{
				// The same event re-entered from inside our own call: a mod hooked the slot after us (its "original" is this
				// thunk) and we took the slot back on top of it (EnsureNotifyHook) -- so it passes the event on to us again.
				// Straight on to what was in the slot when we installed, without our logic: the chain runs once, the other mod
				// stays in it (Dynamic Armor Physics + 4.7.3: an endless us -> it -> us loop, stack overflow on load).
				// Another event sent from inside the call (Notify Events dispatches its fake graph events there, their sinks
				// may send to the player) is a new send: the whole chain, as without the re-hook (4.7.4-4.7.7 sent it straight
				// to `first`: past the other mods and the pose's hold). Too deep = a mod turning events into each other: `first`
				if (depth > 0 && (depth >= kMaxDepth || (calls[depth - 1].holder == a_this && calls[depth - 1].event == a_event.data()))) {
					return first(a_this, a_event);
				}
				if (Poison::DeferPoseExit(a_event)) {
					return true;  // passed to the graph one update later (Poison::PostAnimUpdate)
				}
				calls[depth++] = { a_this, a_event.data() };
				const bool result = func(a_this, a_event);
				--depth;
				if (Poison::Listening()) {
					Poison::OnInputEvent(a_event);
				}
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;   // the slot's previous value (after a re-hook: the other mod's hook)
			static inline REL::Relocation<decltype(thunk)> first;  // the slot's value when we installed (vanilla or earlier mods)

			struct Call
			{
				RE::IAnimationGraphManagerHolder* holder;
				const char*                       event;  // BSFixedString data: one pointer per string in the game's string pool
			};
			static constexpr int                    kMaxDepth = 8;
			static inline thread_local Call         calls[kMaxDepth]{};  // our calls in progress on this thread, innermost last
			static inline thread_local int          depth{ 0 };
		};

		// Ultimate Combat SE (UltimateCombat.dll, SE only) writes its own NotifyAnimationGraph into this very slot on
		// kDataLoaded without keeping the previous one (it calls the vanilla function directly) -- loaded after us it
		// silently removes our hook. Checked every frame: overwritten -> hooked again on top (we then call its hook, it
		// calls vanilla; we see even the events it swallows). Mods that keep us as their original (Dynamic Armor Physics,
		// SkyParkourNG) call us back from inside -- handled in PlayerNotify::thunk (Notify Events too). A few times at most.
		void ReportSlots();

		void EnsureNotifyHook()
		{
			static REL::Relocation<std::uintptr_t> holder{ RE::VTABLE_PlayerCharacter[3] };
			static int                             rehooks = 0;
			const auto                             slot = *reinterpret_cast<const std::uintptr_t*>(holder.address() + sizeof(void*));
			if (slot == reinterpret_cast<std::uintptr_t>(&PlayerNotify::thunk) || rehooks >= 5) {
				return;
			}
			++rehooks;
			HMODULE     module = nullptr;
			wchar_t     name[MAX_PATH]{};
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(slot), &module);
			if (module) {
				GetModuleFileNameW(module, name, MAX_PATH);
			}
			PlayerNotify::func = holder.write_vfunc(0x1, PlayerNotify::thunk);
			logger::warn("NotifyAnimationGraph of the player was replaced by {} -- hooked again on top of it ({}/5)",
				std::filesystem::path(name).filename().string(), rehooks);
		}

		// --- PlayerCharacter::UpdateAnimation (vtbl[0] 0x7D): the graph is touched only right before its update ---

		struct PlayerUpdateAnimation
		{
			static void thunk(RE::PlayerCharacter* a_this, float a_delta)
			{
				Poison::PreAnimUpdate(a_this);
				func(a_this, a_delta);
				Poison::PostAnimUpdate(a_this);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// --- PlayerCharacter::Update (vtbl[0] 0xAD): main thread, every frame the world runs ----------------------

		struct PlayerUpdate
		{
			static void thunk(RE::PlayerCharacter* a_this, float a_delta)
			{
				func(a_this, a_delta);
				ReportSlots();
				EnsureNotifyHook();
				EnsureGraphSinks(a_this);
				Poison::Tick(a_this, a_delta);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// once, at the first frame (every mod is in by then): a slot that is no longer ours = a mod hooked it after us
		// (it must call through to us; Elden Counter / Valhalla do on 0xAD). The NotifyAnimationGraph slot is taken back.
		void ReportSlots()
		{
			static bool done = false;
			if (done) {
				return;
			}
			done = true;
			REL::Relocation<std::uintptr_t> player{ RE::VTABLE_PlayerCharacter[0] };
			REL::Relocation<std::uintptr_t> holder{ RE::VTABLE_PlayerCharacter[3] };
			const auto                      at = [](std::uintptr_t a_vtbl, std::size_t a_idx) {
                return *reinterpret_cast<const std::uintptr_t*>(a_vtbl + a_idx * sizeof(void*));
			};
			const std::tuple<const char*, std::uintptr_t, std::uintptr_t> slots[] = {
				{ "PlayerCharacter::Update (vtbl[0] 0xAD)", at(player.address(), 0xAD), reinterpret_cast<std::uintptr_t>(&PlayerUpdate::thunk) },
				{ "PlayerCharacter::UpdateAnimation (vtbl[0] 0x7D)", at(player.address(), 0x7D), reinterpret_cast<std::uintptr_t>(&PlayerUpdateAnimation::thunk) },
				{ "NotifyAnimationGraph (vtbl[3] 1)", at(holder.address(), 0x1), reinterpret_cast<std::uintptr_t>(&PlayerNotify::thunk) },
			};
			for (auto& [what, now, ours] : slots) {
				if (now == ours) {
					if (Settings::Get().debugLog) {
						logger::info("hook {}: ours is the outermost", what);
					}
				} else {
					logger::info("hook {}: {} was hooked after us and sits on top (it must call ours)", what, Diagnostics::ModuleOf(now));
				}
			}
		}
	}

	void Install()
	{
		REL::Relocation<std::uintptr_t> player{ RE::VTABLE_PlayerCharacter[0] };
		PlayerUpdate::func = player.write_vfunc(0xAD, PlayerUpdate::thunk);
		PlayerUpdateAnimation::func = player.write_vfunc(0x7D, PlayerUpdateAnimation::thunk);
		REL::Relocation<std::uintptr_t> holder{ RE::VTABLE_PlayerCharacter[3] };
		PlayerNotify::func = holder.write_vfunc(0x1, PlayerNotify::thunk);
		PlayerNotify::first = PlayerNotify::func.address();
		// what each hook wraps: not SkyrimSE.exe = another mod hooked it before us (fine if it calls the original)
		logger::info("hooks installed: Update wraps {}, UpdateAnimation wraps {}, NotifyAnimationGraph wraps {}",
			Diagnostics::ModuleOf(PlayerUpdate::func.address()), Diagnostics::ModuleOf(PlayerUpdateAnimation::func.address()),
			Diagnostics::ModuleOf(PlayerNotify::func.address()));

		PendingPoisonOffset();  // resolve and log once at startup
		if (auto src = PoisonedWeaponSource()) {
			src->AddEventSink(PoisonSink::Get());
		}
	}

	void OnGameLoaded()
	{
		std::lock_guard lock(g_sinkLock);
		g_sinkManager = nullptr;
		g_sinkGraphs.clear();
	}
}
