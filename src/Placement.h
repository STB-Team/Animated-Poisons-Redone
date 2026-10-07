#pragma once

// Where the item sits in the hand (our own models, HandItems). Folders under Data/SKSE/Plugins/AnimatedPoisonsRedone:
//   * ThirdPerson -- PoisonList*.json, "floatList": nif name -> pos xyz, rot xyz (degrees, extrinsic Euler), scale,
//     for the bottle, the arrow / bolt projectile and the FX PoisonCloak01.nif alike (3P skeleton, AnimObjectL / AnimObjectR);
//     not listed -> built-in values (bottles: the original mod's table, BuiltinTransforms.inl; arrows / bolts: fitted
//     along the shaft of the loaded model);
//   * FirstPerson -- PoisonList / ArrowList*.json, "floatList" the same way (1P skeleton: AnimObjectL / NPC R Hand [RHnd]);
//     not listed -> built-in values by bottle class, arrows / bolts fitted along the shaft.
// Extra files <list>_<anything>.json are searched first (patches for other mods). Files are re-read when they change on disk.

namespace Placement
{
	struct Transform
	{
		std::array<float, 3> pos{};
		std::array<float, 3> rot{};  // degrees, extrinsic Euler (HandItems)
		float                scale{ 1.0f };
		const char*          source{ "zero" };
		// built-in projectile = the vanilla steel arrow's placement (tip at the model's y = 0, shaft 58.2 along -y);
		// HandItems measures the loaded model's shaft and moves it along its axis so that the hand is at this share of the
		// shaft from the tip, as on the arrow (< 0 = as given)
		float shaftFit{ -1.0f };
	};

	inline constexpr float kRefShaft = 58.2f;  // steelarrowflight.nif: the shaft's length (its bound sphere's diameter)

	// model path of a form the way Papyrus GetWorldModelPath returns it ("" if none)
	std::string ModelPath(const RE::TESForm* a_form);

	// 3P: bottle, projectile (arrow / bolt) or the poison cloak FX
	Transform ForDisplay(const RE::TESForm* a_form);

	// 1P: bottle on AnimObjectL, arrow / bolt on NPC R Hand [RHnd]
	Transform ForDisplay1P(const RE::TESForm* a_form);

}
