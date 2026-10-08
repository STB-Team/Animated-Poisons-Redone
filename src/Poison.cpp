#include "Poison.h"

#include "Flow.h"
#include "HandItems.h"
#include "Diagnostics.h"
#include "Placement.h"
#include "Settings.h"

namespace Poison
{
	namespace
	{
		constexpr auto kModName = "Animated Poisons Redone";

		// ---- forms ------------------------------------------------------------------------------------------

		struct Forms
		{
			RE::TESForm*       cloak{};        // Skyrim 0x1046CD ARTO PoisonCloak01 (the FX)
			RE::TESGlobal*     iiPoison{};     // ImmersiveInteractions.esp 0x149953 AR_Poison: its MCM poison toggle
		} F;

		// ---- player helpers (main thread) -------------------------------------------------------------------

		// runtime info: only with bDebugLog (load, warnings and errors are always logged)
		template <class... Args>
		void Debug(fmt::format_string<Args...> a_fmt, Args&&... a_args)
		{
			if (Settings::Get().debugLog) {
				logger::info(a_fmt, std::forward<Args>(a_args)...);
			}
		}

		RE::PlayerCharacter* P() { return RE::PlayerCharacter::GetSingleton(); }

		bool GBool(const char* a_var)
		{
			bool v = false;
			return P()->GetGraphVariableBool(a_var, v) && v;
		}

		int GInt(const char* a_var)
		{
			std::int32_t v = 0;
			return P()->GetGraphVariableInt(a_var, v) ? v : 0;
		}

		bool FirstPerson() { return GInt("i1stPerson") != 0; }
		bool OMA() { return GBool("bGPMAInstalled") && !Diagnostics::OmaStateMissing(); }  // its pose state too (4.7.11)
		bool Sitting() { return P()->AsActorState()->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal; }

		// Utility.IsInMenuMode (SE 56476 / AE 56833): two flags of the UI (SE 516934 / 516935, AE 403436 / 403437).
		// Skyrim Souls RE unpauses menus by clearing kPausesGame without counting them, so the flags stay off with the
		// inventory open; it marks such menus with 1 << 28 -- open menus that pause the game or carry that mark count too
		bool InMenuMode()
		{
			static REL::Relocation<bool*> f1{ RELOCATION_ID(516934, 403436) };
			static REL::Relocation<bool*> f2{ RELOCATION_ID(516935, 403437) };
			if (*f1 || *f2) {
				return true;
			}
			constexpr std::uint32_t kSoulsUnpaused = 1u << 28;  // Skyrim Souls RE: an unpaused menu
			const auto              ui = RE::UI::GetSingleton();
			if (!ui) {
				return false;
			}
			for (auto& menu : ui->menuStack) {
				if (menu && (menu->PausesGame() || (menu->menuFlags.underlying() & kSoulsUnpaused) != 0)) {
					return true;
				}
			}
			return false;
		}

		// Improved Camera SE: in 1P the 3rd-person body is drawn too, and its arms may be the ones shown -- the items go
		// on the 3P body as well
		bool BodyInFirstPerson()
		{
			static const bool ic = GetModuleHandleW(L"ImprovedCameraSE.dll") != nullptr;
			return ic;
		}

		// which items show in the current view: 3P items (PoisonBottle / PoisonFX / Projectile) in 3P, and in 1P only if the 3P
		// body is drawn there (Improved Camera); 1P items (*1P) only in 1P
		bool ItemVisibleNow(std::string_view a_name)
		{
			return a_name.ends_with("1P") ? FirstPerson() : (!FirstPerson() || BodyInFirstPerson());
		}

		// the camera state (TESCameraState::id): 0 first person, 9 third person, 8 animated, 5 furniture, ...; -1 none
		int CameraState()
		{
			const auto camera = RE::PlayerCamera::GetSingleton();
			const auto state = camera ? camera->currentState.get() : nullptr;
			return state ? static_cast<int>(state->id) : -1;
		}

		// the player's active behavior graph: 0 = 3P (DefaultMale/Female), 1 = 1P (FirstPerson); -1 unknown
		int ActiveGraph()
		{
			RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
			if (!P()->GetAnimationGraphManager(manager) || !manager) {
				return -1;
			}
			return static_cast<int>(manager->GetRuntimeData().activeGraph);
		}

		// attack / block / bash / cast / shout / stagger / jump / killmove in progress: no animation at all
		bool Busy()
		{
			for (auto v : { "IsAttacking", "IsBlocking", "IsBashing", "IsCastingRight", "IsCastingLeft", "IsCastingDual", "IsShouting",
					 "IsStaggering", "IsRecoiling", "bInJumpState", "bIsSynced" }) {
				if (GBool(v)) {
					return true;
				}
			}
			for (auto& v : Settings::Get().interruptVariables) {  // other mods' actions (a dodge in progress ...)
				if (GBool(v.c_str())) {
					return true;
				}
			}
			return false;
		}

		// the OMA pose on, but not ours (ours was on / released within the exit blend): another mod's pose, e.g.
		// Immersive Interactions eating / reading -- the poison plays no animation over it
		std::atomic<float> g_ourPoseAt{ -10.0f };

		bool ForeignPose() { return GBool("bOffsetGPMA") && Flow::Now() - g_ourPoseAt > 0.8f; }

		bool IsInterruptEvent(std::string_view a_tag)
		{
			for (auto& e : Settings::Get().interruptEvents) {
				if (e.size() == a_tag.size() && _strnicmp(e.data(), a_tag.data(), e.size()) == 0) {
					return true;
				}
			}
			return false;
		}

		// the weapon is being drawn or sheathed: waited for, then the drawn / sheathed path
		bool Equipping()
		{
			const auto ws = P()->AsActorState()->GetWeaponState();
			return GBool("IsEquipping") || GBool("IsUnequipping") || (ws != RE::WEAPON_STATE::kSheathed && ws != RE::WEAPON_STATE::kDrawn);
		}

		bool Bow(int a_type) { return a_type == 7 || (a_type == 12 && Settings::Get().crossbow); }


		// what the Nemesis patch stbgpi ends the pose with: attack (incl. bow) / block / bash / cast / shout / drawing -- the
		// pose starts only after the plugin's own draw, so a draw during the pose is the player's
		bool Interrupting()
		{
			for (auto v : { "IsAttacking", "IsBlocking", "IsBashing", "IsCastingRight", "IsCastingLeft", "IsCastingDual", "IsShouting", "IsEquipping" }) {
				if (GBool(v)) {
					return true;
				}
			}
			return false;
		}

		// Actor.GetEquippedItemType: 0 nothing / fists, 7 bow, 12 crossbow, ...
		int EquippedType(bool a_left)
		{
			const auto obj = P()->GetEquippedObject(a_left);
			if (!obj) {
				return 0;
			}
			if (const auto weap = obj->As<RE::TESObjectWEAP>()) {
				switch (weap->GetWeaponType()) {
				case RE::WEAPON_TYPE::kOneHandSword:
					return 1;
				case RE::WEAPON_TYPE::kOneHandDagger:
					return 2;
				case RE::WEAPON_TYPE::kOneHandAxe:
					return 3;
				case RE::WEAPON_TYPE::kOneHandMace:
					return 4;
				case RE::WEAPON_TYPE::kTwoHandSword:
					return 5;
				case RE::WEAPON_TYPE::kTwoHandAxe:
					return 6;
				case RE::WEAPON_TYPE::kBow:
					return 7;
				case RE::WEAPON_TYPE::kStaff:
					return 8;
				case RE::WEAPON_TYPE::kCrossbow:
					return 12;
				default:
					return 0;
				}
			}
			switch (obj->GetFormType()) {
			case RE::FormType::Spell:
				return 9;
			case RE::FormType::Armor:
				return 10;
			case RE::FormType::Light:
				return 11;
			default:
				return 0;
			}
		}

		const char* WeaponStateName(RE::WEAPON_STATE a_ws)
		{
			switch (a_ws) {
			case RE::WEAPON_STATE::kSheathed:
				return "sheathed";
			case RE::WEAPON_STATE::kWantToDraw:
				return "wantToDraw";
			case RE::WEAPON_STATE::kDrawing:
				return "drawing";
			case RE::WEAPON_STATE::kDrawn:
				return "drawn";
			case RE::WEAPON_STATE::kWantToSheathe:
				return "wantToSheathe";
			case RE::WEAPON_STATE::kSheathing:
				return "sheathing";
			default:
				return "?";
			}
		}

		// movement input of the player (keys / stick / auto-move)
		bool MoveInput()
		{
			const auto pc = RE::PlayerControls::GetSingleton();
			return pc && (pc->data.autoMove || std::abs(pc->data.moveInputVec.x) > 0.01f || std::abs(pc->data.moveInputVec.y) > 0.01f);
		}

		float GraphSpeed()
		{
			float speed = 0.0f;
			P()->GetGraphVariableFloat("Speed", speed);
			return speed;
		}

		// for the log: everything a decision of the session depends on
		std::string State()
		{
			if (!Settings::Get().debugLog) {
				return {};
			}
			const auto pc = RE::PlayerControls::GetSingleton();
			const auto in = pc ? pc->data.moveInputVec : RE::NiPoint2{};
			return fmt::format("[weapon {}{}{}, R {} L {}, {}{}{}speed {:.0f}, input {:.2f} {:.2f}{}{}, {} (camera {}), OMA pose {}{}]",
				WeaponStateName(P()->AsActorState()->GetWeaponState()), GBool("IsEquipping") ? " IsEquipping" : "", GBool("IsUnequipping") ? " IsUnequipping" : "",
				EquippedType(false), EquippedType(true), P()->AsActorState()->IsSprinting() ? "sprinting, " : "", P()->IsSneaking() ? "sneaking, " : "",
				Busy() ? "busy, " : "", GraphSpeed(), in.x, in.y, pc && pc->data.autoMove ? " auto-move" : "", P()->IsMoving() ? ", moving" : "",
				FirstPerson() ? "1P" : "3P", CameraState(), GBool("bOffsetGPMA") ? "on" : "off", InMenuMode() ? ", menu" : "");
		}

		// safety: the game's moveStop lost in a blend that ignores the normal graph's events -> legs walk on the spot.
		// No movement input but the graph still at speed -> moveStop to the graph (in idle it changes nothing).
		void MoveStopSafety(const char* a_when)
		{
			if (MoveInput() || GraphSpeed() < 5.0f || GBool("IsAttacking")) {  // never into an attack / bow draw
				return;
			}
			P()->NotifyAnimationGraph("moveStop");
			Debug("safety ({}): no movement input but graph speed {:.0f} -> moveStop sent", a_when, GraphSpeed());
		}

		bool HasKeyword(const RE::TESForm* a_form, std::string_view a_editorID)
		{
			const auto kw = a_form ? a_form->As<RE::BGSKeywordForm>() : nullptr;
			return kw && kw->HasKeywordString(a_editorID);
		}

		// NetImmerse.Get/SetNodeScale(PlayerRef, a_node, false): third-person 3D
		RE::NiAVObject* Node3P(const char* a_node)
		{
			const auto root = P()->Get3D(false);
			return root ? root->GetObjectByName(a_node) : nullptr;
		}

		std::optional<float> NodeScale(const char* a_node)
		{
			const auto n = Node3P(a_node);
			return n ? std::optional<float>(n->local.scale) : std::nullopt;
		}

		void SetNodeScale(const char* a_node, float a_scale)
		{
			if (const auto n = Node3P(a_node)) {
				n->local.scale = a_scale;
				RE::NiUpdateData ud;
				n->Update(ud);
			}
		}

		void Notify(const char* a_text)
		{
			RE::DebugNotification(a_text);
		}

		// ---- requests from the sink -------------------------------------------------------------------------

		std::mutex                       g_reqLock;
		std::optional<RE::AlchemyItem*>  g_request;
		std::atomic<bool>                g_busy{ false };    // request pending or session running
		std::atomic<bool>                g_listen{ false };
		std::atomic<float>               g_sessionEndAt{ -10.0f };  // bDebugLog logs the graph events 2 s after a session
		std::atomic<bool>                g_afterLoad{ false };
		bool                             g_toldNoPose{ false };
		float                            g_postCheckAt{ -1.0f };  // moveStop safety after the pose ended (main thread)
		float                            g_setupCheckAt{ -1.0f };  // the install check, a few seconds after a load

		// repeat: poison applied in the last fRepeatWindow of the pose -> one more pose right after it
		std::atomic<float>               g_repeatFrom{ -1.0f };  // window of the running pose (session clock), -1 = closed
		std::atomic<float>               g_repeatUntil{ -1.0f };
		RE::AlchemyItem*                 g_repeat{ nullptr };    // queued (under g_reqLock), at most one

		// handed from a session to its repeat (under g_reqLock)
		struct Carry
		{
			bool                 chain{ false };    // the pose is still on: the repeat restarts its clip (echo), no new start event
			bool                 sheathe{ false };  // the weapon was drawn for the chain (draw safety at its end)
			std::optional<float> rightScale;        // original node scales, still hidden (0.01) by the previous session
			std::optional<float> leftScale;
			bool                 rightScaled{ false };
			bool                 leftScaled{ false };
		};
		Carry g_carry;

		// restart the pose's clip in place -- hkbClipGenerator::startEcho (vtbl 0x1B, SE 58610) on the graph thread
		std::atomic<float> g_echoRequest{ -1.0f };  // echo duration requested by the session, < 0 = none
		std::atomic<float> g_echoResume{ 0.0f };    // clip time the echoed clip resumes at (0 = from the start)
		std::atomic<int>   g_echoResult{ 0 };       // 0 pending, 1 done, -1 clip not found

		struct GraphEvent
		{
			float       t;
			std::string tag;
			std::string payload;
		};
		constexpr auto          kInputMark = "<sent>";  // payload of an event sent TO the graph (NotifyAnimationGraph hook)
		std::mutex              g_evLock;
		std::vector<GraphEvent> g_events;      // while a session runs
		std::deque<GraphEvent>  g_preEvents;   // bDebugLog: the last second before a session
		std::atomic<bool>       g_sessionOn{ false };

		// An action started in the pose. The OMA pose (GPMAOffsetState) hangs the same RootBehaviorGraph under its
		// bone switch, so entering / leaving it restarts the weapon branch (Weap_Readied_State exits on entry -- DisableBumper --
		// and enters again on exit -- EnableBumper). An action event in the pose entered its state first and the exit then
		// restarted the branch: the state exited at once and was entered again (its enter notify twice), and
		//   - the bow: Bow_AttackState's exit notify raises attackRelease (1hm_behavior) -> the draw released itself;
		//   - an action stbgpi does not leave the pose on (BFCO attacks, TK dodges, TUDM rolls -- kInterruptEvents): our stop
		//     came 0.1 s later and the restart cut the action already running.
		// So such an event (stbgpi's kPoseExitEvents + kInterruptEvents, sent to the graph by anyone: the engine's actions incl.
		// idles -- AIProcess::SetupSpecialIdle 38290 -> TESActionData::DoIt --, DLLs, Papyrus) is held back in the pose: the
		// pose is released now, the event goes to the graph after the next graph update -- the branch has restarted, the action
		// starts once, after it. Events sent meanwhile wait behind it (order kept). The caller gets true, as the pose's own
		// wildcard took these events before.
		std::recursive_mutex             g_heldLock;  // StopPose re-enters the hook (APR_PoisonStop) on this thread
		std::vector<RE::BSFixedString>   g_held;

		// ---- session ----------------------------------------------------------------------------------------

		struct Pose
		{
			bool        check{ false };  // watch the OMA pose end
			float       sentAt{ 0.0f };
			bool        entered{ false };
			float       enteredAt{ 0.0f };
			bool        seen{ false };    // bOffsetGPMA seen on
			float       exitAt{ -1.0f };  // last AnimObjectUnequip after the pose started
			bool        exitEvent{ false };
			bool        interrupted{ false };
			bool        moveChecked{ false };  // moveStop safety done after the entry blend
			bool        released{ false };     // released by the plugin on an action stbgpi does not know
			float       releaseAt{ -1.0f };    // our stop for it, unless the graph's own exit comes first
			int         graph{ -1 };           // active graph index when last seen (view switch = it changes)
			float       switchedAt{ -1.0f };   // when the view switched during the pose (its blends are not an exit)
			std::vector<char> varPrev;         // interruptVariables last frame (an interrupt = false -> true)
		};

		struct Ctx
		{
			Carry                                              carry;            // from the session it repeats
			bool                                               handedOff{ false };  // the pose goes on in the repeat: scales stay hidden
			float                                              t0{ 0.0f };
			bool                                               animSet{ false };
			std::vector<std::string>                           displays;  // hand items shown by this session
			std::optional<float>                               rightScale;
			std::optional<float>                               leftScale;
			bool                                               rightScaled{ false };
			bool                                               leftScaled{ false };
			Pose                                               pose;
		};


		Ctx        g_ctx;
		Flow::Task g_task;
		bool       g_running{ false };

		template <class... Args>
		void Log(fmt::format_string<Args...> a_fmt, Args&&... a_args)
		{
			if (Settings::Get().debugLog) {
				logger::info("[{:7.3f}] {}", Flow::Now() - g_ctx.t0, fmt::format(a_fmt, std::forward<Args>(a_args)...));
			}
		}

		// the clip choice and the speed for OAR are variables of the player's graphs, declared by the mod's Nemesis patch
		// stbgpi (3P 0_master and 1P _1stperson/0_master, as OMA declares bOffsetGPMA):
		//   APR_PoisonAnim  int   -- 60 bottle, 61 arrow / bolt, 0 no session: OAR's sub-mod condition (CompareValues)
		//   APR_PoisonSpeed float -- fAnimSpeed: OAR's SetPlaybackSpeedMultiplier on the clip's activation
		// written in every graph of the player (3P and 1P), so a view switch finds them set; OAR reads them through the
		// actor (the active graph). Through the actor: IAnimationGraphManagerHolder::SetGraphVariable* (SE 32142 / 32143, AE
		// 32886 / 32887) runs ForEachAnimationGraph itself. NOT BShkbAnimationGraph::SetGraphVariable* of CommonLibSSE-NG: its
		// SE ids are wrong (63607 is a Havok function on 1.5.97) and crash the game
		constexpr auto kAnimVar = "APR_PoisonAnim";
		constexpr auto kSpeedVar = "APR_PoisonSpeed";

		// the patch's variables in the active graph (= Nemesis / Pandora run with the mod)
		bool PoseVars()
		{
			int   anim = 0;
			float speed = 0.0f;
			return P()->GetGraphVariableInt(kAnimVar, anim) && P()->GetGraphVariableFloat(kSpeedVar, speed);
		}

		float g_animResetAt{ -1.0f };  // APR_PoisonAnim back to 0 once the pose has faded out (at the latest then)

		void SetAnim(int a_value)
		{
			if (a_value != 0) {
				g_animResetAt = -1.0f;  // a new pose takes the variable over
			}
			const bool set = P()->SetGraphVariableInt(kAnimVar, a_value);
			g_ctx.animSet = a_value != 0 && set;
			Log("{} = {}{}", kAnimVar, a_value, set ? "" : " (not set in every graph)");
		}

		// 0 at the session's stop would let OAR put OMA's placeholder clip back while the pose still fades (the clip loops
		// / re-activates): its own OffsetGPMAStop annotation (0.2 s) would cut a new pose started in that window. The
		// variable stays until the pose is gone (bOffsetGPMA off), at most 1.5 s
		void ResetAnimAfterPose() { g_animResetAt = Flow::Now() + 1.5f; }

		void WatchAnimReset()
		{
			if (g_animResetAt < 0.0f || g_running) {
				return;
			}
			if (!GBool("bOffsetGPMA") || Flow::Now() >= g_animResetAt) {
				g_animResetAt = -1.0f;
				SetAnim(0);
			}
		}

		void SetSpeed(float a_value)
		{
			const bool set = P()->SetGraphVariableFloat(kSpeedVar, a_value);
			Log("{} = {:g}{}", kSpeedVar, a_value, set ? "" : " (not set in every graph)");
		}

		void RestoreScales()
		{
			// author: a saved 0.01 is a leftover of an earlier run -> 1
			auto fix = [](float s) { return std::abs(s - 0.01f) < 1e-4f ? 1.0f : s; };
			if (g_ctx.rightScaled && g_ctx.rightScale) {
				SetNodeScale("Weapon", fix(*g_ctx.rightScale));
			}
			if (g_ctx.leftScaled && g_ctx.leftScale) {
				SetNodeScale("Shield", fix(*g_ctx.leftScale));
			}
			if (g_ctx.rightScaled || g_ctx.leftScaled) {
				Log("shield / bow shown");
			}
			g_ctx.rightScaled = g_ctx.leftScaled = false;
		}

		// an item on the 3P skeleton (HandItems)
		void CreateDisplay(RE::TESForm* a_form, const char* a_name, const char* a_node)
		{
			const auto t = Placement::ForDisplay(a_form);
			const auto model = Placement::ModelPath(a_form);
			const bool ok = HandItems::Create(P(), a_name, model, a_node, t, a_form && a_form->GetFormType() == RE::FormType::Projectile);
			Log("item {} {}: {:08X} {} on {} pos {:g} {:g} {:g} rot {:g} {:g} {:g} scale {:g} [{}]", a_name, ok ? "created" : "FAILED",
				a_form ? a_form->GetFormID() : 0, model, a_node, t.pos[0], t.pos[1], t.pos[2], t.rot[0], t.rot[1], t.rot[2], t.scale, t.source);
		}

		// the same item on the 1P skeleton
		void CreateDisplay1P(RE::TESForm* a_form, const char* a_name, const char* a_node)
		{
			const auto t = Placement::ForDisplay1P(a_form);
			const auto model = Placement::ModelPath(a_form);
			const bool ok = HandItems::Create(P(), a_name, model, a_node, t, a_form && a_form->GetFormType() == RE::FormType::Projectile, true);
			Log("item {} {}: {:08X} {} on 1P {} pos {:g} {:g} {:g} rot {:g} {:g} {:g} scale {:g} [{}]", a_name, ok ? "created" : "FAILED",
				a_form ? a_form->GetFormID() : 0, model, a_node, t.pos[0], t.pos[1], t.pos[2], t.rot[0], t.rot[1], t.rot[2], t.scale, t.source);
		}

		// show / hide an item; "on" is kept for the other view too and shown when the view changes (ItemVisibleNow)
		void EnableDisplay(const char* a_name, bool a_on)
		{
			const bool visible = ItemVisibleNow(a_name);
			HandItems::Show(a_name, a_on && visible);
			auto& d = g_ctx.displays;
			if (a_on) {
				if (std::find(d.begin(), d.end(), a_name) == d.end()) {
					d.emplace_back(a_name);
				}
			} else {
				d.erase(std::remove(d.begin(), d.end(), a_name), d.end());
			}
			Log("item {} {}{}", a_name, a_on ? "on" : "off", a_on && !visible ? " (other view: shown when the view changes)" : "");
		}

		// the view changed during the session -- each item that is on shows in its own view only
		void UpdateDisplayVisibility()
		{
			static int last = -1;
			const int  view = FirstPerson() ? 1 : 0;
			if (view == last) {
				return;
			}
			last = view;
			for (auto& name : g_ctx.displays) {
				HandItems::Show(name.c_str(), ItemVisibleNow(name));
			}
		}

		void HideDisplays()
		{
			const auto list = g_ctx.displays;
			for (auto& name : list) {
				EnableDisplay(name.c_str(), false);
			}
			HandItems::RemoveAll();  // detached at once, the next session creates its own
		}

		bool Send(const char* a_event)
		{
			const bool taken = P()->NotifyAnimationGraph(a_event);
			Log("-> {}{}", a_event, taken ? "" : " (not taken by the graph)");
			return taken;
		}

		// the mod's own start / stop events (Nemesis patch stbgpi). Not taken = not in the graph or the graph cannot enter the
		// pose right now: no animation this time (a missing patch is reported by the install check)
		bool SendStart()
		{
			if (Send("APR_PoisonStart")) {
				return true;
			}
			static bool told = false;
			if (!told) {
				told = true;
				logger::warn("APR_PoisonStart not taken by the behavior graph -- no animation this time");
			}
			return false;
		}

		// not taken = the graph is not in the pose any more (normal); a missing patch is reported by the graph check at load
		void SendStop() { Send("APR_PoisonStop"); }

		void LogEvents(const std::vector<GraphEvent>& a_events, const char* a_what)
		{
			if (!Settings::Get().debugLog) {
				return;
			}
			for (auto& e : a_events) {
				logger::info("[{:7.3f}] {} {} {}", e.t - g_ctx.t0, a_what, e.tag, e.payload);
			}
		}

		bool StartPose()
		{
			g_ctx.pose = Pose{};
			g_ctx.pose.check = true;
			g_ctx.pose.sentAt = Flow::Now();
			std::vector<GraphEvent> stale;
			{
				std::lock_guard lock(g_evLock);
				stale.swap(g_events);
			}
			LogEvents(stale, "graph event");
			g_listen = true;
			if (!SendStart()) {  // no APR_PoisonStart in the graph: no pose at all
				g_ctx.pose.check = false;
				return false;
			}
			return true;
		}

		bool PoseOn() { return g_ctx.pose.check && GBool("bOffsetGPMA"); }

		// release the pose ourselves: from here on its end is not an interrupt
		void StopPose()
		{
			SendStop();
			g_ctx.pose.check = false;
			g_ourPoseAt = Flow::Now();
			g_postCheckAt = Flow::Now() + Settings::Get().moveStopCheck + 0.5f;  // after the exit blend (0.5 s)
		}

		void LogBoneSwitch();  // diagnostics, below

		// every frame before the session step: did the pose end on its own (attack / block / cast -> stbgpi)?
		void WatchPose()
		{
			auto& p = g_ctx.pose;
			std::vector<GraphEvent> events;
			{
				std::lock_guard lock(g_evLock);
				events.swap(g_events);
			}
			const auto& st = Settings::Get();
			LogEvents(events, "graph event");
			// a view switch during the pose. The start event went to every graph of the manager, so the new graph is normally
			// in the pose as well; if it is not, it enters it now
			if (p.check) {
				const int g = ActiveGraph();
				if (p.graph < 0) {
					p.graph = g;
				} else if (g >= 0 && g != p.graph) {
					p.graph = g;
					p.switchedAt = Flow::Now();
					const bool on = GBool("bOffsetGPMA");
					Log("view switched to {} during the pose: the pose in the new graph is {}", g == 1 ? "1P" : "3P", on ? "on, goes on" : "off -> entered again");
					if (!on) {
						SendStart();
						p.seen = false;  // "pose gone" waits until it is on again
					}
					UpdateDisplayVisibility();
				}
			}
			const bool switching = p.switchedAt >= 0.0f && Flow::Now() - p.switchedAt < 0.4f;
			for (auto& [t, tag, payload] : events) {
				if (!p.check || p.interrupted) {
					continue;
				}
				if (payload != kInputMark && _stricmp(tag.c_str(), "OffsetGPMAStop") == 0 && Flow::Now() - p.sentAt < 0.4f) {
					// OMA's own placeholder clip raises OffsetGPMAStop at 0.20 s: our OAR clip did not replace it (OAR not
					// loaded, or the mod's OAR folders missing). Log only: one odd frame must not alarm the player
					static bool told = false;
					if (!told) {
						told = true;
						logger::error("the pose is OMA's own placeholder clip (it ends itself at 0.2 s): the mod's animation was not "
									  "applied by Open Animation Replacer -- is OAR loaded for this game version, are the mod's OAR "
									  "folders installed?");
					}
				}
				if (IsInterruptEvent(tag)) {
					// an action of another mod (dodge, BFCO attack ...) that stbgpi does not leave the pose on
					p.interrupted = true;
					p.released = true;
					Log("pose interrupted: {} {} -> released", tag, payload == kInputMark ? "sent to the graph" : "raised by the graph");
					continue;
				}
				if (_stricmp(tag.c_str(), "AnimObjLoad") == 0 && _stricmp(payload.c_str(), "AnimObjectGPMA") == 0) {
					if (!p.entered) {
						p.entered = true;
						p.enteredAt = Flow::Now();
						Log("pose entered ({:.0f} ms after the start event)", (p.enteredAt - p.sentAt) * 1000.0f);
					}
				} else if (p.entered && (_stricmp(tag.c_str(), "BeginWeaponDraw") == 0 || _stricmp(tag.c_str(), "weaponDraw") == 0)) {
					// the player draws during the pose (a bow on the back): stbgpi leaves the pose on WeapEquip; if not,
					// the exit confirm releases the pose ourselves
					p.exitEvent = true;
					p.interrupted = true;
					Log("pose interrupted: the player draws ({})", tag);
				} else if (_stricmp(tag.c_str(), "AnimObjectUnequip") == 0 && p.entered && Flow::Now() - p.enteredAt > st.exitGuard && !switching) {
					p.exitAt = Flow::Now();  // GPMAOffsetState exit notify -- or someone else's annotation: confirmed below
				}
			}
			// interruptVariables turning on during the pose (on already when it started = not this action's)
			const auto& vars = st.interruptVariables;
			if (p.check) {
				const bool first = p.varPrev.size() != vars.size();
				p.varPrev.resize(vars.size());
				for (std::size_t i = 0; i < vars.size(); ++i) {
					const bool on = GBool(vars[i].c_str());
					if (on && !first && !p.varPrev[i] && !p.interrupted) {
						p.interrupted = true;
						p.released = true;
						Log("pose interrupted: {} turned on -> released", vars[i]);
					}
					p.varPrev[i] = on;
				}
			}
			if (p.released && p.check) {
				// the graph may already be leaving the pose itself (stbgpi's exit, its notify AnimObjectUnequip is here): a stop
				// sent into that blend would be a second transition on top of it -> nothing is sent. Otherwise our own stop
				// after 0.1 s, if no exit came meanwhile.
				if (p.exitAt >= 0.0f) {
					// as an stbgpi exit: the session end checks after fExitConfirm that the pose is really gone (else our stop)
					p.released = false;
					p.exitEvent = true;
					Log("pose interrupted: the graph leaves the pose itself (exit notify) -> no stop sent {}", State());
					return;
				}
				if (p.releaseAt < 0.0f) {
					p.releaseAt = Flow::Now() + 0.1f;
				} else if (Flow::Now() >= p.releaseAt) {
					StopPose();  // stbgpi does not leave the pose on these: our own stop
					Log("pose released {}", State());
				}
				return;
			}
			// exit notify + an action of the stbgpi set at once = interrupted at the start of the 0.2 s blend
			if (p.check && !p.interrupted && p.exitAt >= 0.0f) {
				if (Interrupting()) {
					p.exitEvent = true;
					p.interrupted = true;
					Log("pose exit (AnimObjectUnequip + action): interrupted");
				} else if (Flow::Now() - p.exitAt > st.exitWindow) {
					p.exitAt = -1.0f;
				}
			}
			if (p.check && p.entered && !p.moveChecked && Flow::Now() - p.enteredAt >= st.moveStopCheck) {
				p.moveChecked = true;
				Log("after the pose's entry blend {}", State());
				LogBoneSwitch();
				MoveStopSafety("pose start");
			}
			if (p.check && !p.interrupted) {
				if (GBool("bOffsetGPMA")) {
					p.seen = true;
				} else if (p.seen && !switching) {
					p.interrupted = true;
					Log("pose gone (bOffsetGPMA off): interrupted");
				}
			}
		}

		bool RepeatQueued()
		{
			std::lock_guard lock(g_reqLock);
			return g_repeat != nullptr;
		}

		// wait until session time a_t; true if the pose was interrupted first
		Flow::Until Interrupted(float a_t)
		{
			return Flow::Until{ [] { return g_ctx.pose.interrupted; }, a_t - Flow::Now() };
		}

		// The loaded bolt of a crossbow in the hand. The engine hangs a clone of the ammo's Arrow0 on NPC R MagicNode [RMag]
		// (Actor::AttachArrow 37590: bolts -> get_magicnode 38804, arrows -> WEAPON), not on the crossbow; the crossbow clips
		// put RMag into the groove, the pose's clips do not. For the pose the real bolt is hidden (app-culled) and, in the
		// bottle pose, a copy of it hangs on WEAPON at the bolt's place relative to the crossbow -- it moves with the
		// crossbow, no per-frame writes. RMag itself is never touched: DetachArrow 37591 empties it, Fire 17693 launches
		// from it. Both skeletons (3P and 1P), so a view switch keeps it.
		struct BoltCopy
		{
			RE::NiPointer<RE::NiAVObject> real;
			RE::NiPointer<RE::NiNode>     weapon;
			RE::NiPointer<RE::NiAVObject> copy;
		};
		std::vector<BoltCopy> g_boltCopies;
		float                 g_boltReleaseAt{ -1.0f };  // released once the pose has faded out (bOffsetGPMA off), at the latest then

		// a_copy false (bCrossbow on: the bolt pose, our own bolt in the hand, the crossbow scaled away): the loaded bolt is
		// only hidden
		void HoldBolt(bool a_copy)
		{
			if (!g_boltCopies.empty()) {  // a new pose while the last one still fades out: the held bolt stays held
				g_boltReleaseAt = -1.0f;
				Log("loaded bolt still held from the last pose");
				return;
			}
			for (int fp = 0; fp < 2; ++fp) {
				const auto root = P()->Get3D(fp == 1);
				const auto rmagObj = root ? root->GetObjectByName("NPC R MagicNode [RMag]") : nullptr;
				const auto rmag = rmagObj ? rmagObj->AsNode() : nullptr;
				const auto weaponObj = root ? root->GetObjectByName("WEAPON") : nullptr;
				const auto weapon = weaponObj ? weaponObj->AsNode() : nullptr;
				if (!rmag || !weapon) {
					continue;
				}
				for (auto& child : rmag->GetChildren()) {
					// the engine's clone keeps the quiver node's name: Arrow0
					if (!child || _strnicmp(child->name.c_str(), "Arrow", 5) != 0 || child->GetAppCulled()) {
						continue;
					}
					RE::NiPointer<RE::NiAVObject> copy;
					if (a_copy) {
						copy.reset(child->Clone());
						if (!copy) {
							continue;
						}
						copy->name = "APR_LoadedBolt";
						copy->local = weapon->world.Invert() * child->world;
						weapon->AttachChild(copy.get(), true);
						RE::NiUpdateData update{};
						copy->UpdateDownwardPass(update, 0);
					}
					child->SetAppCulled(true);
					g_boltCopies.push_back({ RE::NiPointer<RE::NiAVObject>(child.get()), RE::NiPointer<RE::NiNode>(weapon), copy });
					Log("crossbow in the hand: the loaded bolt ({}) hidden{} for the pose ({})", child->name.c_str(), a_copy ? ", its copy on WEAPON" : "", fp ? "1P" : "3P");
				}
			}
		}

		void ReleaseBolt()
		{
			if (g_boltCopies.empty()) {
				return;
			}
			for (auto& b : g_boltCopies) {
				if (b.copy && b.copy->parent) {
					b.copy->parent->DetachChild2(b.copy.get());
				}
				if (b.real) {
					b.real->SetAppCulled(false);  // already fired / detached by the engine: a detached node, harmless
				}
			}
			g_boltCopies.clear();
			g_boltReleaseAt = -1.0f;
			Log("loaded bolt shown again, its copy removed");
		}

		// the pose's exit blend (0.5 s) still holds RMag in the pose's place: released when the pose is gone, or at once
		// when the engine took the real bolt off meanwhile (fired: DetachArrow empties RMag)
		void ReleaseBoltAfterPose() { g_boltReleaseAt = g_boltCopies.empty() ? -1.0f : Flow::Now() + 1.0f; }

		void WatchBoltRelease()
		{
			if (g_boltReleaseAt < 0.0f) {
				return;
			}
			const bool fired = std::any_of(g_boltCopies.begin(), g_boltCopies.end(), [](auto& b) { return b.real && !b.real->parent; });
			if (fired || !GBool("bOffsetGPMA") || Flow::Now() >= g_boltReleaseAt) {
				ReleaseBolt();
			}
		}

		void Restore()
		{
			if (g_ctx.handedOff) {
				HandItems::RetireAll(3);  // shown until the repeat's own items are up (3 frames of overlap)
			} else {
				HideDisplays();
			}
			if (!g_ctx.handedOff) {
				RestoreScales();
				ReleaseBoltAfterPose();
			}
			if (g_ctx.animSet) {
				ResetAnimAfterPose();
			}
		}

		// ---- the pose's clip ----------------------------------------------------------------------------------

		// hkbBehaviorGraph::activeNodes (+0x98) is hkArray<hkbNodeInfo>*; CommonLibSSE-NG types it as an opaque
		// hkRefVariant and has no hkbNodeInfo, so only what we read is declared here (Havok layout, same on SE and AE)
		struct NodeInfo
		{
			std::byte    syncInfo[0x50];  // 00 hkbGeneratorSyncInfo
			RE::hkbNode* nodeTemplate;    // 50
			RE::hkbNode* nodeClone;       // 58
			std::byte    rest[0x30];      // 60
		};
		static_assert(offsetof(NodeInfo, nodeClone) == 0x58 && sizeof(NodeInfo) == 0x90);

		// the pose's clip in a graph (OMA GPMAOffsetClip, 3P and 1P); graph thread only
		RE::hkbClipGenerator* PoseClip(RE::BShkbAnimationGraph* a_graph)
		{
			if (!a_graph || !a_graph->behaviorGraph) {
				return nullptr;
			}
			const auto nodes = reinterpret_cast<RE::hkArray<NodeInfo>*>(a_graph->behaviorGraph->activeNodes.get());
			if (!nodes) {
				return nullptr;
			}
			static const auto vtbl = RE::VTABLE_hkbClipGenerator[0].address();
			for (auto& info : *nodes) {
				const auto node = info.nodeClone;
				if (node && *reinterpret_cast<std::uintptr_t*>(node) == vtbl && node->name.c_str() &&
					_stricmp(node->name.c_str(), "GPMAOffsetClip") == 0) {
					return static_cast<RE::hkbClipGenerator*>(node);
				}
			}
			return nullptr;
		}

		// Diagnostics (bDebugLog): the OMA upper-body switch as the engine sees it. BSBoneSwitchGeneratorUtils::generateInternal
		// (SE 62402) applies a child's pose only if numData (bones in the pose) <= its bone weight count -- the pose then
		// silently does not show (Pandora 5.0 beta). Offsets from the SE database types (Havok layout, same on AE):
		// BSBoneSwitchGenerator +0x50 pDefaultGenerator, +0x58 ChildrenA (hkArray: data, size); BoneData +0x30 pGenerator,
		// +0x38 bone weights; hkbBoneWeightArray +0x30 weights (hkArray<float>); hkbCharacter +0x50 setup, +0x98 numPoseLocal;
		// hkbCharacterSetup +0x20 skeleton; hkaSkeleton +0x28 bones (hkArray)
		void LogBoneSwitch()
		{
			if (!Settings::Get().debugLog) {
				return;
			}
			RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
			if (!P()->GetAnimationGraphManager(manager) || !manager) {
				return;
			}
			const auto rd = [](std::uintptr_t a_base, std::ptrdiff_t a_off) { return a_base ? *reinterpret_cast<std::uintptr_t*>(a_base + a_off) : 0; };
			const auto ri = [](std::uintptr_t a_base, std::ptrdiff_t a_off) { return a_base ? *reinterpret_cast<std::int32_t*>(a_base + a_off) : -1; };
			for (std::uint32_t i = 0; i < manager->graphs.size(); ++i) {
				const auto graph = manager->graphs[i].get();
				if (!graph || !graph->behaviorGraph) {
					continue;
				}
				const auto character = reinterpret_cast<std::uintptr_t>(&graph->characterInstance);
				const auto setup = rd(character, 0x50);
				const auto skeleton = rd(setup, 0x20);
				const int  bones = ri(skeleton, 0x30);
				const int  poseLocal = ri(character, 0x98);
				std::string children;
				bool        found = false;
				if (const auto nodes = reinterpret_cast<RE::hkArray<NodeInfo>*>(graph->behaviorGraph->activeNodes.get())) {
					for (auto& info : *nodes) {
						const auto node = info.nodeClone;
						if (!node || !node->name.c_str() || _stricmp(node->name.c_str(), "xComState_BoneSwitchGenerator") != 0) {
							continue;
						}
						found = true;
						const auto self = reinterpret_cast<std::uintptr_t>(node);
						const auto data = rd(self, 0x58);
						const int  count = ri(self, 0x60);
						for (int c = 0; data && c < count; ++c) {
							const auto boneData = rd(data, c * 8);
							const auto weights = rd(boneData, 0x38);
							children += fmt::format(" child {}: generator {}, weights {}", c, rd(boneData, 0x30) ? "yes" : "NO",
								weights ? std::to_string(ri(weights, 0x38)) : std::string("NO ARRAY"));
						}
						children += fmt::format(" (default generator {})", rd(self, 0x50) ? "yes" : "NO");
						break;
					}
				}
				logger::info("bone switch [{}] {}: skeleton bones {}, pose bones {}; xComState_BoneSwitchGenerator {}{}", i,
					graph->projectName.c_str() ? graph->projectName.c_str() : "?", bones, poseLocal, found ? "active:" : "not active", children);
			}
		}

		// Character property bone weights shorter than the skeleton (v49, Pandora 5.0 beta: its character template has 99
		// weights per array, a skeleton like XP32 has 126 bones). BSBoneSwitchGeneratorUtils::generateInternal (SE 62402)
		// skips a child whose weights are fewer than the pose's bones, so OMA's upper body (property UpperBody) never shows
		// in 3P. Padded here once per array, in place: a new bone takes its parent's weight (a root: 0); arrays for another
		// bone set (shorter than the vanilla 99, e.g. SneakMagic* 85) and long-enough ones (Nemesis) are left alone. The
		// character data is shared by every actor of that project -- they all get the same, correct length. The new buffer
		// is marked "do not deallocate" for Havok and never freed (a few arrays per project, once).
		// Offsets (SE database types): hkbCharacter +0x50 setup; hkbCharacterSetup +0x20 skeleton, +0x40 character data;
		// hkaSkeleton +0x18 parentIndices (hkArray<int16>), +0x28 bones; hkbCharacterData +0x80 property values;
		// hkbVariableValueSet +0x30 variant values (hkArray<hkReferencedObject*>); hkbBoneWeightArray +0x30 weights (hkArray<float>)
		void PadBoneWeights()
		{
			RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
			if (!P()->GetAnimationGraphManager(manager) || !manager) {
				return;
			}
			constexpr int kVanillaBones = 99;
			static const auto weightsVtbl = RE::VTABLE_hkbBoneWeightArray[0].address();
			const auto rd = [](std::uintptr_t a_base, std::ptrdiff_t a_off) { return a_base ? *reinterpret_cast<std::uintptr_t*>(a_base + a_off) : 0; };
			const auto ri = [](std::uintptr_t a_base, std::ptrdiff_t a_off) { return a_base ? *reinterpret_cast<std::int32_t*>(a_base + a_off) : 0; };
			for (auto& graphPtr : manager->graphs) {
				const auto graph = graphPtr.get();
				// human 3P projects only: the 99 threshold is the vanilla human skeleton; a werewolf / vampire lord / horse
				// project (the player transformed at load) has its own skeleton and arrays -- left alone (4.7.7)
				const auto project = graph && graph->projectName.c_str() ? std::string_view(graph->projectName.c_str()) : std::string_view{};
				if (_strnicmp(project.data(), "DefaultMale", 11) != 0 && _strnicmp(project.data(), "DefaultFemale", 13) != 0) {
					continue;
				}
				const auto setup = rd(reinterpret_cast<std::uintptr_t>(&graph->characterInstance), 0x50);
				const auto skeleton = rd(setup, 0x20);
				const auto data = rd(setup, 0x40);
				const int  bones = ri(skeleton, 0x30);
				const auto parents = reinterpret_cast<const std::int16_t*>(rd(skeleton, 0x18));
				const int  parentCount = ri(skeleton, 0x20);
				const auto values = rd(data, 0x80);
				const auto variants = reinterpret_cast<std::uintptr_t*>(rd(values, 0x30));
				const int  variantCount = ri(values, 0x38);
				if (!parents || parentCount < bones || !variants || bones <= kVanillaBones) {
					continue;
				}
				int padded = 0;
				int from = 0;
				for (int v = 0; v < variantCount; ++v) {
					const auto obj = variants[v];
					if (!obj || *reinterpret_cast<std::uintptr_t*>(obj) != weightsVtbl) {
						continue;
					}
					auto&     weights = *reinterpret_cast<float**>(obj + 0x30);
					auto&     size = *reinterpret_cast<std::int32_t*>(obj + 0x38);
					auto&     capacity = *reinterpret_cast<std::int32_t*>(obj + 0x3C);
					const int old = size;
					if (!weights || old < kVanillaBones || old >= bones) {
						continue;
					}
					auto buffer = new float[static_cast<std::size_t>(bones)];  // owned by the character data from now on
					std::copy(weights, weights + old, buffer);
					for (int b = old; b < bones; ++b) {
						const int parent = parents[b];
						buffer[b] = parent >= 0 && parent < b ? buffer[parent] : 0.0f;
					}
					weights = buffer;
					std::atomic_thread_fence(std::memory_order_release);
					capacity = static_cast<std::int32_t>(0x80000000u | static_cast<std::uint32_t>(bones));  // DONT_DEALLOCATE
					size = bones;
					from = old;
					++padded;
				}
				if (padded > 0) {
					logger::info("bone weights of {}: {} arrays padded {} -> {} (the skeleton's bones; Pandora's character template)",
						graph->projectName.c_str() ? graph->projectName.c_str() : "?", padded, from, bones);
				}
			}
		}

		// ---- the session: checks, draw, the pose, its end; a chained repeat restarts the clip in place ----------

		Flow::Task PoisonFlow(RE::AlchemyItem* a_poison)
		{
			const auto& st = Settings::Get();
			auto&       c = g_ctx;
			const auto  model = Placement::ModelPath(a_poison);
			const bool  chain = c.carry.chain;
			Log("poison {:08X} {} '{}', speed {:g}{}{} {}", a_poison->GetFormID(), model, a_poison->GetName(), st.animSpeed, chain ? ", chained" : "",
				c.carry.sheathe ? ", drawn for it" : "", State());

			bool sheathe = c.carry.sheathe;
			bool bow = false;
			bool bowDrawn = false;
			if (chain) {
				// the previous session left the shield / bow hidden for this one: take its saved originals first, so that
				// every exit below (Finish -> Restore) shows them again
				c.rightScale = c.carry.rightScale;
				c.leftScale = c.carry.leftScale;
				c.rightScaled = c.carry.rightScaled;
				c.leftScaled = c.carry.leftScaled;
				// the previous pose is still on and this session owns it: release it if nothing is played
				if (!PoseVars() || Busy()) {
					Log("skip: busy, the chained pose is released");
					StopPose();
					co_return;
				}
				bow = Bow(EquippedType(false));
				bowDrawn = bow && c.carry.rightScaled;
			} else {
				// not installed: no animation, logged once (the player is told by the install check)
				if (!PoseVars() || !OMA()) {
					Log("skip: {} not in the behavior graph", !OMA() ? "Offset Movement Animation" : "the mod's Nemesis patch");
					if (!g_toldNoPose) {
						g_toldNoPose = true;
						logger::error("no animation: {} is not in the player's behavior graph (run Nemesis / Pandora with OMA and the mod ticked)",
							!OMA() ? "Offset Movement Animation" : "the mod's Nemesis patch (APR_PoisonAnim / APR_PoisonSpeed)");
					}
					co_return;
				}
				if (HasKeyword(a_poison, "AR_IgnoreObject") || GInt("SkyparkourOngoing") == 1) {
					Log("skip: AR_IgnoreObject / parkour");
					co_return;
				}
				if (ForeignPose()) {
					Log("skip: another mod's OMA pose is playing (Immersive Interactions eating / reading ...), no animation");
					co_return;
				}
				// an action already playing is never cut and nothing is played after it either
				if (Busy()) {
					Log("skip: busy (attack / block / cast / ...), no animation");
					co_return;
				}
				// drawing / sheathing does not cancel it -- wait for the end, then the drawn or the sheathed path
				if (Equipping()) {
					Log("drawing / sheathing in progress: waiting");
					const bool done = co_await Flow::Until{ [] { return Busy() || !Equipping(); }, st.waitEquip };
					if (!done || Busy()) {
						Log("skip: {} while waiting for draw / sheathe", done ? "busy" : "timeout");
						co_return;
					}
					Log("weapon {}", P()->AsActorState()->IsWeaponDrawn() ? "drawn" : "sheathed");
				}

				const int right = EquippedType(false);
				bow = Bow(right);
				if (!P()->AsActorState()->IsWeaponDrawn() && bow) {
					// bow / crossbow on the back is not drawn -- the arrow / bolt pose right away, the bow stays there
					Log("bow on the back: no draw");
				} else if (!P()->AsActorState()->IsWeaponDrawn()) {
					// draw, the pose starts as soon as the weapon is in the hand
					if (!st.drawSheathed || P()->IsOnMount() || Sitting() || P()->AsActorState()->IsSwimming() ||
						(EquippedType(true) == 0 && right == 0)) {
						Log("skip: sheathed and cannot draw");
						co_return;
					}
					// in a menu: waits only with bPlayInMenus off; with Skyrim Souls the game runs and DrawWeaponMagicHands
					// (40232, the Draw action) has no menu check -- it draws in the menu
					if (!st.playInMenus) {
						co_await Flow::Until{ [] { return !InMenuMode(); } };
					}
					if (Busy() || Equipping()) {
						Log("skip: busy before drawing, no animation");
						co_return;
					}
					P()->DrawWeaponMagicHands(true);
					Log("draw weapon");
					co_await Flow::Until{ [] { return P()->AsActorState()->IsWeaponDrawn(); }, st.drawTimeout };
					Log("weapon in the hand {}", State());
					// wait for the whole draw (WeapEquip_Out clears IsEquipping): a pose started over its tail can swallow
					// WeapEquip_Out and leave the graph in the draw
					const bool drawEnded = co_await Flow::Until{ [] { return !GBool("IsEquipping"); }, st.drawTail };
					if (!P()->AsActorState()->IsWeaponDrawn()) {
						Log("skip: weapon not drawn {}", State());
						co_return;
					}
					sheathe = true;
					Log("draw {}: pose starts", drawEnded ? "ended" : "still ending (timeout)");
				}
				bowDrawn = bow && P()->AsActorState()->IsWeaponDrawn();
			}

			// bPlayInMenus off: the pose waits for the menu to close -- and the state is checked again after it
			if (!chain && !st.playInMenus && InMenuMode()) {
				co_await Flow::Until{ [] { return !InMenuMode(); } };
				if (Busy() || ForeignPose()) {
					Log("skip: busy after the menu closed, no animation");
					co_return;
				}
			}

			PadBoneWeights();  // the graphs of this load (a no-op once padded)
			SetAnim(60);
			if (!chain && !bow && EquippedType(false) == 12 && P()->AsActorState()->IsWeaponDrawn()) {
				HoldBolt(true);
			}

			if (!chain) {  // chained: still hidden by the previous session, its saved originals taken above
				c.rightScale = NodeScale("Weapon");
				c.leftScale = NodeScale("Shield");
				SetNodeScale("Shield", 0.01f);
				c.leftScaled = true;
			}

			CreateDisplay(a_poison, "PoisonBottle", "AnimObjectL");  // crafted poisons too: the model path is enough
			CreateDisplay(F.cloak, "PoisonFX", "AnimObjectR");
			EnableDisplay("PoisonBottle", true);
			CreateDisplay1P(a_poison, "PoisonBottle1P", "AnimObjectL");
			EnableDisplay("PoisonBottle1P", true);

			if (bow) {  // bow / crossbow: the arrow / bolt is poisoned
				SetAnim(61);
				const auto ammo = P()->GetCurrentAmmo();
				if (!ammo || !ammo->data.projectile) {
					Log("bow without ammo: no arrow in the hand");
				} else {
					CreateDisplay(ammo->data.projectile, "Projectile", "AnimObjectR");
					CreateDisplay1P(ammo->data.projectile, "Projectile1P", "NPC R Hand [RHnd]");
					EnableDisplay("Projectile1P", true);
					EnableDisplay("Projectile", true);
				}
				if (bowDrawn && !c.rightScaled) {  // the bow in the hand is hidden for the pose; on the back it stays visible
					SetNodeScale("Weapon", 0.01f);
					c.rightScaled = true;
					if (EquippedType(false) == 12) {
						HoldBolt(false);  // the loaded bolt hangs on RMag, not on the crossbow -- hidden with it
					}
				}
			}

			// OAR multiplies the clip's speed by APR_PoisonSpeed when it activates (an ActiveClip of that activation only,
			// reset on deactivate): written before every start. The clip's own times scale
			// with it; the interrupt guards do not (behavior blends and game states, not clip time). An echo keeps the
			// activation, so a chained repeat keeps its speed.
			const float speed = st.animSpeed;
			SetSpeed(speed);
			bool echoed = false;
			if (chain) {
				// no stop / start events (0.5 s blend out of the OMA state, the clip is shared by all its sub-states): restart
				// the clip in place at the applying part, the old pose fading out over the repeat blend
				g_echoResult = 0;
				g_echoResume = st.repeatResumeAt;
				g_echoRequest = st.repeatBlend / speed;  // the clip's blend, at the clip's speed
				co_await Flow::Until{ [] { return g_echoResult != 0; }, 0.25f };
				echoed = g_echoResult == 1;
				if (echoed) {
					c.pose = Pose{};
					c.pose.check = true;
					c.pose.sentAt = c.pose.enteredAt = Flow::Now();
					c.pose.entered = true;  // the clip's own AnimObjectUnequip at t = 0 is not an exit (fExitGuard)
					c.pose.seen = true;
					{
						std::lock_guard lock(g_evLock);
						g_events.clear();
					}
					g_listen = true;
					Log("clip restarted in place at {:g} s, the old pose fading out over {:g} s", st.repeatResumeAt, st.repeatBlend / speed);
				} else {
					g_echoRequest = -1.0f;
					Log("pose clip not found: release and start again");
					StopPose();
					co_await Flow::Until{ [] { return !GBool("bOffsetGPMA"); }, 1.0f };
				}
			}
			if (!echoed) {
				// the previous pose may still be fading out (0.5 s after its stop): the start event only takes the state back
				// with its clip running on, so the clip is restarted too (echo) -- a fresh start right away
				if (ForeignPose()) {
					Log("skip: another mod's OMA pose started meanwhile (Immersive Interactions eating / reading ...), no animation");
					co_return;
				}
				const bool fading = !chain && GBool("bOffsetGPMA");
				Log("pose starts {}", State());
				if (!StartPose()) {
					Log("skip: the behavior graph has no APR_PoisonStart (Nemesis / Pandora not run with the mod's patch)");
					co_return;
				}
				if (fading) {
					g_echoResult = 0;
					g_echoResume = 0.0f;
					g_echoRequest = st.repeatBlend / speed;
					co_await Flow::Until{ [] { return g_echoResult != 0; }, 0.25f };
					Log("previous pose still fading: clip {}", g_echoResult == 1 ? "restarted (echo)" : "not found");
					g_echoRequest = -1.0f;
				}
			} else {
				Log("pose goes on {}", State());
			}
			// a chained repeat resumes its clip at repeatResumeAt -- its times count from where the clip would have started
			const auto t0 = Flow::Now() - (echoed ? st.repeatResumeAt / speed : 0.0f);
			if (st.repeatWindow > 0.0f) {
				// every session (chain): the last share of the visible pose [start, items hidden], taken until the pose is
				// released
				g_repeatFrom = t0 + st.poisonHideAt * (1.0f - st.repeatWindow) / speed;
				g_repeatUntil = t0 + st.poisonStopAt / speed;
			}
			bool ok = !co_await Interrupted(t0 + st.poisonFxAt / speed);
			if (ok) {
				EnableDisplay("PoisonFX", true);
				ok = !co_await Interrupted(t0 + st.poisonHideAt / speed);
			}
			// a repeat queued by now goes on at once -- the hands are still applying: nothing hidden, the next session takes
			// the pose over and resumes the clip at the applying part (no lowering / raising, the bow not shown)
			if (!(ok && RepeatQueued())) {
				HideDisplays();  // APR_PoisonAnim stays until the pose has faded out (Restore -> ResetAnimAfterPose)
				if (ok) {  // until the stop -- or a repeat taken in [hide, stop] goes on right away
					co_await Flow::Until{ [] { return g_ctx.pose.interrupted || RepeatQueued(); }, t0 + st.poisonStopAt / speed - Flow::Now() };
					ok = !g_ctx.pose.interrupted;
				}
			}
			g_repeatFrom = -1.0f;  // the pose is over (or interrupted): no more repeats for it

			bool handOff = false;
			{
				std::lock_guard lock(g_reqLock);
				if (!ok && g_repeat) {
					Log("interrupted: queued repeat dropped");
					g_repeat = nullptr;
				}
				if (ok && g_repeat) {
					// the pose goes on in the repeat: no stop, scales stay hidden
					handOff = true;
					g_carry = Carry{ true, sheathe, c.rightScale, c.leftScale, c.rightScaled, c.leftScaled };
				}
			}
			if (handOff) {
				c.handedOff = true;
				g_listen = false;
				Log("repeat queued: the pose goes on");
				co_return;
			}
			if (ok) {
				StopPose();
			}
			Log("pose end ({}) {}", ok ? "done" : "interrupted", State());
			RestoreScales();
			if (!ok && c.pose.exitEvent) {
				co_await Flow::Until{ [] { return !PoseOn(); }, st.exitConfirm };
				if (PoseOn()) {
					Log("the exit event was not an exit, pose still on");
					StopPose();
					ok = true;
				}
			}
			g_listen = false;

			// the weapon drawn for the poison stays in the hand. Safety: drawn but IsEquipping still on after the pose (its
			// WeapEquip_Out swallowed, standing in the draw's end pose) -> WeapEquip_Out sent, as the clip's annotation would
			if (sheathe && ok && P()->AsActorState()->GetWeaponState() == RE::WEAPON_STATE::kDrawn && GBool("IsEquipping") && !GBool("IsUnequipping")) {
				const bool ended = co_await Flow::Until{ [] { return !GBool("IsEquipping") || P()->AsActorState()->GetWeaponState() != RE::WEAPON_STATE::kDrawn; },
					st.equipStuck };
				if (!ended && GBool("IsEquipping") && P()->AsActorState()->GetWeaponState() == RE::WEAPON_STATE::kDrawn) {
					P()->NotifyAnimationGraph("WeapEquip_Out");
					Log("safety: draw stuck (IsEquipping {:g} s after the pose) -> WeapEquip_Out sent {}", st.equipStuck, State());
				}
			}
			if (sheathe) {
				Log("weapon drawn for the poison stays in the hand {}", State());
			}
		}

		bool Poisoned(RE::InventoryEntryData* a_entry, RE::AlchemyItem* a_poison)
		{
			if (!a_entry || !a_entry->object || a_entry->object->GetFormType() != RE::FormType::Weapon || !a_entry->extraLists) {
				return false;
			}
			for (auto xList : *a_entry->extraLists) {
				const auto xp = xList ? xList->GetByType<RE::ExtraPoison>() : nullptr;
				if (xp && xp->poison == a_poison) {
					return true;
				}
			}
			return false;
		}

		// the left-hand weapon carries this poison and the right one does not (same poison on both -> right, as vanilla)
		bool PoisonedLeft(RE::AlchemyItem* a_poison)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return false;
			}
			const bool right = Poisoned(player->GetEquippedEntryData(false), a_poison);
			const bool left = Poisoned(player->GetEquippedEntryData(true), a_poison);
			Debug("poison {:08X} found on: right {}, left {}", a_poison->GetFormID(), right, left);
			return left && !right;
		}

		// ---- session control --------------------------------------------------------------------------------

		void Finish(const char* a_why, bool a_keepBusy = false)
		{
			Restore();
			g_listen = false;
			Log("session end ({})", a_why);
			g_sessionOn = false;
			g_sessionEndAt = Flow::Now();
			g_running = false;
			g_busy = a_keepBusy;  // a repeat follows at once: new poisons meanwhile are not taken as fresh sessions
		}

		void Start(RE::AlchemyItem* a_poison, const Carry& a_carry = {})
		{
			g_ctx = Ctx{};
			g_ctx.carry = a_carry;
			g_ctx.t0 = Flow::Now();
			std::deque<GraphEvent> pre;
			{
				std::lock_guard lock(g_evLock);
				pre.swap(g_preEvents);
				g_events.clear();
			}
			g_sessionOn = true;
			if (Settings::Get().debugLog && !pre.empty()) {
				LogEvents(std::vector<GraphEvent>(pre.begin(), pre.end()), "before:");
			}
			g_running = true;
			g_task = PoisonFlow(a_poison);
		}

		// Animated Poisons (Nexus 72849) is incompatible: it has no switch -- the whole mod is its poison animation, so two
		// animations would play
		bool AnimatedPoisonsLoaded()
		{
			const auto dh = RE::TESDataHandler::GetSingleton();
			return dh && (dh->LookupLoadedModByName("Animated Poisons.esp") || dh->LookupLoadedLightModByName("Animated Poisons.esp"));
		}

		// a normal player graph (3P DefaultMale / DefaultFemale, 1P FirstPerson) is active -- not a werewolf / vampire lord
		// / horse one, which has no OMA and no patch of ours
		bool NormalGraph()
		{
			RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
			if (!P()->GetAnimationGraphManager(manager) || !manager || manager->GetRuntimeData().activeGraph >= manager->graphs.size()) {
				return false;
			}
			const auto graph = manager->graphs[manager->GetRuntimeData().activeGraph].get();
			const auto name = graph && graph->projectName.c_str() ? std::string_view(graph->projectName.c_str()) : std::string_view{};
			return _strnicmp(name.data(), "DefaultMale", 11) == 0 || _strnicmp(name.data(), "DefaultFemale", 13) == 0 ||
			       _strnicmp(name.data(), "FirstPerson", 11) == 0;
		}

		// the install check: 3 s of game time after a load (graphs and mods settled), on a normal graph; a problem is checked
		// once more 3 s later and only then reported -- the player is told only what is clearly not installed, once per game
		// start. Everything else goes to the log only
		int g_setupFailures{ 0 };

		void SetupCheck()
		{
			if (!NormalGraph()) {
				g_setupCheckAt = Flow::Now() + 3.0f;  // a werewolf / horse graph now: later
				return;
			}
			struct Problem
			{
				const char* log;
				const char* notice;
			};
			std::vector<Problem> problems;
			bool                 dummy = false;
			if (!Diagnostics::OarLoaded()) {
				problems.push_back({ "Open Animation Replacer is not loaded -- no animation",
					"Animated Poisons Redone: Open Animation Replacer is not loaded" });
			} else if (Diagnostics::OmaStateMissing() || !P()->GetGraphVariableBool("bOffsetGPMA", dummy)) {
				// no OMA variable, or (4.7.11) its pose state 1233 missing from Master_Behavior while our transition leads
				// there (repaired at load, Diagnostics::RepairGraph): OMA's patch is not in the generated behavior
				problems.push_back({ "Offset Movement Animation is not in the behavior -- tick it in Nemesis / Pandora together with Animated Poisons Redone",
					"Offset Movement Animation is not in the behavior - tick it in Nemesis / Pandora together with Animated Poisons Redone" });
			} else if (!PoseVars()) {
				problems.push_back({ "the mod's Nemesis patch is not in the behavior -- run Nemesis / Pandora with the mod ticked",
					"Animated Poisons Redone: run Nemesis with the mod ticked" });
			}
			if (AnimatedPoisonsLoaded()) {
				problems.push_back({ "Animated Poisons is loaded -- incompatible (two poison animations), disable it",
					"Animated Poisons Redone: disable Animated Poisons - incompatible" });
			}
			if (problems.empty()) {
				logger::info("install check: ok");
				g_setupFailures = 0;
				return;
			}
			if (++g_setupFailures < 2) {  // confirm 3 s later before telling anyone
				g_setupCheckAt = Flow::Now() + 3.0f;
				return;
			}
			g_setupFailures = 0;
			static bool told = false;
			for (auto& pr : problems) {
				logger::error("install check: {}", pr.log);
				if (!told) {
					Notify(pr.notice);
				}
			}
			told = true;
		}
	}

	void OnDataLoaded()
	{
		F.cloak = RE::TESForm::LookupByID(0x1046CD);
		if (!F.cloak) {
			logger::warn("PoisonCloak01 (Skyrim.esm 0x1046CD) not found: no poison FX");
		}
		if (const auto dh = RE::TESDataHandler::GetSingleton(); dh && dh->LookupLoadedModByName("ImmersiveInteractions.esp")) {
			F.iiPoison = dh->LookupForm<RE::TESGlobal>(0x149953, "ImmersiveInteractions.esp");
			if (F.iiPoison) {
				logger::info("Immersive Interactions: works alongside, its poison animation (AR_Poison, now {:g}) is turned off on every poison", F.iiPoison->value);
			} else {
				logger::warn("Immersive Interactions: AR_Poison (0x149953) not found -- an unknown version, it may animate poisons too");
			}
		}
	}

	void OnPreLoad()
	{
		ReleaseBolt();
		{
			std::lock_guard lock(g_heldLock);
			g_held.clear();  // held events belong to the graph being unloaded
		}
		if (g_running) {
			g_task.Reset();
			Finish("game load");
		}
		std::lock_guard lock(g_reqLock);
		g_request.reset();
		g_repeat = nullptr;
		g_carry = Carry{};
		g_repeatFrom = -1.0f;
		g_echoRequest = -1.0f;
		g_postCheckAt = -1.0f;
		g_busy = false;
	}

	void OnGameLoaded()
	{
		OnPreLoad();
		HandItems::RemoveAll();
		g_afterLoad = true;
	}

	void Tick(RE::PlayerCharacter*, float a_delta)
	{
		Flow::Advance(a_delta);
		HandItems::Tick();
		WatchBoltRelease();
		WatchAnimReset();

		if (g_afterLoad.exchange(false)) {
			// a session saved mid-way: the graphs may come back with the clip variable set (the hand items do not -- our own nodes)
			int anim = 0;
			if (P()->GetGraphVariableInt(kAnimVar, anim) && anim != 0) {
				logger::info("{} {} from the save reset", kAnimVar, anim);
				SetAnim(0);
			}
			g_setupCheckAt = Flow::Now() + 3.0f;
			PadBoneWeights();
		}
		if (g_setupCheckAt >= 0.0f && Flow::Now() >= g_setupCheckAt) {
			g_setupCheckAt = -1.0f;
			SetupCheck();
		}

		if (g_running) {
			if (g_ctx.pose.check && GBool("bOffsetGPMA")) {
				g_ourPoseAt = Flow::Now();
			}
			UpdateDisplayVisibility();
			WatchPose();
			g_task.Step();
			if (!g_task.Running()) {
				// a poison applied at the end of the pose plays once more, right away
				RE::AlchemyItem* repeat = nullptr;
				Carry            carry;
				{
					std::lock_guard lock(g_reqLock);
					std::swap(repeat, g_repeat);
					carry = std::exchange(g_carry, Carry{});
				}
				Finish("done", repeat != nullptr);
				if (repeat) {
					Start(repeat, carry);
					Log("repeat session");
					g_task.Step();
					if (!g_task.Running()) {
						Finish("done");
					}
				}
			}
			return;
		}

		if (g_postCheckAt >= 0.0f && Flow::Now() >= g_postCheckAt) {
			g_postCheckAt = -1.0f;
			Debug("after the pose's exit blend {}", State());
			MoveStopSafety("pose end");
		}

		if (!g_busy) {  // a request is always flagged busy: nothing to take
			return;
		}
		std::optional<RE::AlchemyItem*> req;
		{
			std::lock_guard lock(g_reqLock);
			req.swap(g_request);
		}
		if (req && *req) {
			Start(*req);
			g_task.Step();
			if (!g_task.Running()) {
				Finish("done");
			}
		} else if (req) {
			g_busy = false;
		}
	}

	void OnPoisoned(RE::AlchemyItem* a_poison)
	{
		if (!a_poison) {
			return;
		}
		// Immersive Interactions (and New Anims) work alongside, but poisons are ours: their script reads AR_Poison in
		// OnItemRemoved, a Papyrus event queued by the RemoveItem just before this one -> it reads 0
		if (F.iiPoison && F.iiPoison->value != 0.0f) {
			F.iiPoison->value = 0.0f;
			logger::info("Immersive Interactions: its poison animation turned off (AR_Poison 0) -- poisons are animated by {}", kModName);
		}
		// which weapon got it -- vanilla poisons only the right hand, some mods poison the left one; the event comes right
		// after PoisonObject, so the ExtraPoison tells. The pose smears the right hand: on the left weapon no animation.
		if (PoisonedLeft(a_poison)) {
			Debug("poison {:08X} went onto the LEFT-hand weapon: no animation (the pose smears the right hand; vanilla poisons only the right one)",
				a_poison->GetFormID());
			return;
		}
		// one animation per burst -- uses during it neither restart nor queue, except in the last fRepeatWindow of a pose
		// -> one repeat right after it
		if (g_busy) {
			const float now = Flow::Now();
			const float from = g_repeatFrom;
			std::lock_guard lock(g_reqLock);
			if (!g_repeat && from >= 0.0f && now >= from && now <= g_repeatUntil) {
				g_repeat = a_poison;
				Debug("poison {:08X} applied at the end of the pose: repeat queued", a_poison->GetFormID());
			} else {
				Debug("poison {:08X} applied during an animation: no animation", a_poison->GetFormID());
			}
			return;
		}
		std::lock_guard lock(g_reqLock);
		g_request = a_poison;
		g_busy = true;
	}



	bool IsListed(const std::vector<std::string>& a_list, std::string_view a_event)
	{
		for (auto& e : a_list) {
			if (e.size() == a_event.size() && _strnicmp(e.data(), a_event.data(), e.size()) == 0) {
				return true;
			}
		}
		return false;
	}

	bool DeferPoseExit(const RE::BSFixedString& a_event)
	{
		std::lock_guard lock(g_heldLock);
		if (!g_held.empty()) {
			g_held.push_back(a_event);  // behind the held one
			return true;
		}
		if (!g_running || !g_ctx.pose.check || !a_event.c_str()) {
			return false;
		}
		const std::string_view ev(a_event.c_str());
		const auto&            st = Settings::Get();
		if (!IsListed(st.poseExitEvents, ev) && !IsListed(st.interruptEvents, ev)) {
			return false;
		}
		if (!GBool("bOffsetGPMA")) {
			return false;
		}
		g_ctx.pose.interrupted = true;
		StopPose();
		g_held.push_back(a_event);
		Log("pose interrupted: {} -> the pose released first, the event held until the next graph update", ev);
		return true;
	}

	void PostAnimUpdate(RE::PlayerCharacter* a_player)
	{
		std::vector<RE::BSFixedString> held;
		{
			std::lock_guard lock(g_heldLock);
			if (g_held.empty()) {
				return;
			}
			held.swap(g_held);
		}
		for (auto& e : held) {
			const bool taken = a_player->NotifyAnimationGraph(e);
			Log("held {} sent to the graph{}", e.c_str(), taken ? "" : " (not taken)");
		}
	}

	void PreAnimUpdate(RE::PlayerCharacter* a_player)
	{
		const float resume = g_echoResume;
		const float blend = g_echoRequest.exchange(-1.0f);
		if (blend < 0.0f) {
			return;
		}
		// in every graph of the player (3P and 1P): a view switch during the repeat must find the clip restarted too
		RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
		if (!a_player->GetAnimationGraphManager(manager) || !manager) {
			g_echoResult = -1;
			return;
		}
		const auto active = manager->GetRuntimeData().activeGraph;
		bool       inActive = false;
		for (std::uint32_t i = 0; i < manager->graphs.size(); ++i) {
			const auto clip = PoseClip(manager->graphs[i].get());
			if (!clip) {
				continue;
			}
			// through the vtable, so OAR's hook (ActiveClip::OnEcho) sees it: same replacement and speed multiplier kept
			using StartEcho_t = void (*)(RE::hkbClipGenerator*, float);
			const auto vtbl = *reinterpret_cast<std::uintptr_t**>(clip);
			reinterpret_cast<StartEcho_t>(vtbl[0x1B])(clip, blend);
			if (resume > 0.0f) {
				// setLocalTime (58609, slot 0x1A): clip time = |speed| * a_time + start, speed with OAR's multiplier
				using SetLocalTime_t = void (*)(RE::hkbClipGenerator*, float);
				const float speed = std::fabs(clip->playbackSpeed) > 0.001f ? std::fabs(clip->playbackSpeed) : 1.0f;
				const float before = clip->localTime;
				reinterpret_cast<SetLocalTime_t>(vtbl[0x1A])(clip, resume / speed);
				// an echo keeps its time as an offset from the clip's local time (startEcho 58610): moved with the clip it
				// would show a later pose -> a jump instead of a blend. Offsets back by the same step: the echo stays the
				// old pose
				const float step = clip->localTime - before;
				auto        echoes = *reinterpret_cast<float**>(reinterpret_cast<std::uintptr_t>(clip) + 0xE0);  // {offset, weight, dw/dt, -}
				const auto  count = *reinterpret_cast<std::int32_t*>(reinterpret_cast<std::uintptr_t>(clip) + 0xE8);
				for (std::int32_t e = 0; echoes && e < count; ++e) {
					echoes[e * 4] -= step;
				}
			}
			inActive = inActive || i == active;
		}
		g_echoResult = inActive ? 1 : -1;
	}

	void OnGraphEvent(const RE::BSFixedString& a_tag, const RE::BSFixedString& a_payload)
	{
		PushEvent(a_tag.c_str(), a_payload.c_str());
	}

	void OnInputEvent(const RE::BSFixedString& a_event)
	{
		PushEvent(a_event.c_str(), kInputMark);
	}

	void PushEvent(const char* a_tag, const char* a_payload)
	{
		const float     now = Flow::Now();
		std::lock_guard lock(g_evLock);
		if (g_sessionOn) {
			if (g_events.size() < 512) {
				g_events.push_back({ now, a_tag ? a_tag : "", a_payload ? a_payload : "" });
			}
		} else if (Settings::Get().debugLog) {
			if (now - g_sessionEndAt < 2.0f) {  // what the graph does right after the pose
				logger::info("[{:5.2f}] after the session: graph event {}{}", now - g_sessionEndAt, a_tag ? a_tag : "",
					a_payload == kInputMark ? " <sent>" : (a_payload && *a_payload ? fmt::format(" ({})", a_payload) : std::string{}));
			}
			g_preEvents.push_back({ now, a_tag ? a_tag : "", a_payload ? a_payload : "" });
			while (!g_preEvents.empty() && (g_preEvents.front().t < now - 1.0f || g_preEvents.size() > 256)) {
				g_preEvents.pop_front();
			}
		}
	}

	bool Listening() { return g_listen || Settings::Get().debugLog; }
}
