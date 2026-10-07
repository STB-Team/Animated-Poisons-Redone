#pragma once

// AnimatedPoisonsRedone.ini (Data/SKSE/Plugins), section [Settings]: only what a player needs. Everything else is
// fixed here with the values tested in game; keys of earlier versions are ignored.

struct Settings
{
	// ---- ini [Settings]
	float animSpeed{ 1.0f };     // fAnimSpeed: pose playback speed (0.5 ... 3.0) -> graph variable APR_PoisonSpeed -> OAR
	float repeatWindow{ 0.4f };  // fRepeatWindow: poison applied in this last share of the pose -> one more pose right after it (0 = off)
	bool  drawSheathed{ true };  // bDrawSheathed: sheathed melee weapon -- draw it, then poison (it stays in the hand)
	bool  crossbow{ false };     // bCrossbow: crossbow like the bow (off = the author's: the bottle pose, crossbow in the hand)
	bool  playInMenus{ true };   // bPlayInMenus: with unpaused menus (Skyrim Souls) the pose plays while the inventory is open
	bool  debugLog{ false };     // bDebugLog: session steps + graph events; off = load, warnings and errors only
	// Actions that end the pose (fixed, not in the INI: other mods are added here on request). During the pose
	//   interruptEvents / poseExitEvents -- sent to the player's graph: held back until the pose is released
	//                          (Poison::DeferPoseExit); interruptEvents raised by the graph (clip annotations) also end it;
	//   interruptVariables  -- a graph bool turns on (false -> true): the pose is released; also "busy" when a poison is
	//                          applied (no pose). Names are case-insensitive.
	std::vector<std::string> interruptEvents;
	std::vector<std::string> poseExitEvents;
	std::vector<std::string> interruptVariables;
	static constexpr auto    kInterruptEvents =
		"TKDodgeForward, TKDodgeBack, TKDodgeLeft, TKDodgeRight, TKDR_DodgeStart, "                     // TK Dodge RE (sent / raised)
		"RollStart, RollTrigger, SidestepTrigger, ForwardRollStart, BackwardRollStart, Left_RollStart, Right_RollStart, "
		"Left_ForwardRollStart, Right_ForwardRollStart, Left_BackwardRollStart, Right_BackwardRollStart, "  // The Ultimate Dodge Mod
		"SneakSprintStartRoll, "                                                                          // vanilla sneak roll
		"BFCOAttackstart_0, BFCOAttackstart_1, BFCOAttackStart_Comb, BfcoAttackSwim, "                    // BFCO normal attacks
		// the player staggered / knocked into bleedout -- by the engine or any mod (Parry for all, Unblockable / Undodgeable
		// Hits, Valhalla ...): staggerStart / bleedOutStart are RootBehaviorGraph wildcards (0_master), the very graph under the
		// pose's bone switch -> held like an attack, the stagger plays whole after the pose. StaggerPlayer(Bow): Jump Behavior
		// Overhaul's 0_master; MountedStaggerStart: riding. recoilStart / recoilLargeStart: wildcards of 1HM_Behavior without a
		// condition (taken under the pose with a one-handed weapon; Valhalla, Parry for all send them to a defender). poise_*:
		// the stagger events of the poise mods' Nemesis patches (Loki POISE, Chocolate Poise, MaxsuPoise, Valhalla, Elden
		// Parry). Ragdoll / paralysis / death leave the pose by themselves (Master wildcards)
		"staggerStart, StaggerPlayer, StaggerPlayerBow, MountedStaggerStart, bleedOutStart, recoilStart, recoilLargeStart, "
		"poise_small_start, poise_small_start_fwd, poise_med_start, poise_med_start_fwd, "
		"poise_large_start, poise_large_start_fwd, poise_largest_start, poise_largest_start_fwd";
	// IsAttacking: any attack (a backup for attack events that do not pass the hook); bIsDodging / bInIframe: TK Dodge RE;
	// IsStaggering / IsRecoiling / IsBleedingOut: a stagger, recoil or bleedout set without an event through the hook
	static constexpr auto kInterruptVariables = "IsAttacking, bIsDodging, bInIframe, IsStaggering, IsRecoiling, IsBleedingOut";
	// the events the Nemesis patch stbgpi leaves the pose on (Master wildcards -> Root while bOffsetGPMA) -- keep in sync
	// with EVENTS in the mod's build.py
	static constexpr auto kPoseExitEvents =
		"attackStart attackStartLeftHand attackStartDualWield "
		"attackPowerStartInPlace attackPowerStartForward attackPowerStartBackward attackPowerStartLeft attackPowerStartRight "
		"attackPowerStartInPlaceLeftHand attackPowerStartForwardLeftHand attackPowerStartRightLeftHand "
		"attackPowerStartLeftLeftHand attackPowerStartBackLeftHand attackPowerStartDualWield "
		"attackStartSprint attackStartSprintLeftHand attackPowerStart_Sprint attackPowerStart_SprintLeftHand "
		"attackPowerStart_2HWSprint attackPowerStart_2HMSprint AttackPower2HMForwardSprint AttackPower2HWForwardSprint "
		"AttackStartH2HRight AttackStartH2HLeft attackPowerStartForwardH2HRightHand attackPowerStartForwardH2HLeftHand "
		"attackPowerStartH2HCombo bashStart bashPowerStart BlockBashSprint blockStart bowAttackStart "
		"BeginCastLeft BeginCastRight BeginCastVoice WeapEquip Magic_Equip";

	// ---- fixed
	static constexpr float moveStopCheck{ 0.3f };   // moveStop safety: checked this long after the pose started and after it ended
	// clip time in seconds at speed 1 (divided by fAnimSpeed)
	static constexpr float poisonFxAt{ 0.5f };      // poison cloak FX shown
	static constexpr float poisonHideAt{ 1.8f };    // bottle / arrow / FX hidden
	static constexpr float poisonStopAt{ 2.0f };    // pose released
	static constexpr float repeatBlend{ 0.45f };    // a repeat restarts the clip in place; the old pose fades out over this (clip echo)
	static constexpr float repeatResumeAt{ 0.7f };  // ... at this clip time (the bottle in the hand, applying)
	static constexpr float waitEquip{ 3.0f };       // weapon being drawn / sheathed: wait up to this long, then play
	static constexpr float drawTimeout{ 4.0f };     // give up if the weapon drawn for the poison is not out by then
	static constexpr float drawTail{ 3.0f };        // the whole draw (until WeapEquip_Out) is waited for before the pose
	static constexpr float equipStuck{ 0.5f };      // after the pose: drawn but IsEquipping still on this long -> WeapEquip_Out
	// the pose ended by the graph (attack / block / cast / draw -> Nemesis patch stbgpi)
	static constexpr float exitGuard{ 0.05f };      // AnimObjectUnequip right after the pose started is the clip's own annotation
	static constexpr float exitWindow{ 0.3f };      //   ... an exit counts only with an attack / block / bash / cast / shout / draw within this
	static constexpr float exitConfirm{ 0.35f };    // after such an exit the pose must be gone by then, else it is released anyway

	static Settings& Get()
	{
		static Settings s;
		return s;
	}

	void Load();
};
