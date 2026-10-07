#pragma once

// The poison-applying animation (GiraPomba's clips from Immersive Interactions - New Anims) played through Offset
// Movement Animation, without Immersive Interactions and without Papyrus. One session at a time.
//
//   * poison applied: PoisonedWeapon::Event (sent by the confirm callback SE 39407 / AE 40482 after PoisonObject +
//     RemoveItem, while PlayerCharacter::pendingPoison still holds the poison);
//   * the pose: the mod's Nemesis patch stbgpi -- events APR_PoisonStart / APR_PoisonStop, graph variables
//     APR_PoisonAnim / APR_PoisonSpeed read by the OAR sub-mods; OMA's state GPMAOffsetState (enter notify
//     AnimObjLoad "AnimObjectGPMA", exit notify AnimObjectUnequip) and its bOffsetGPMA;
//   * an action during the pose (attack, block, cast, other mods' dodges / attacks): held back in the
//     NotifyAnimationGraph hook until the pose is released (DeferPoseExit); graph events / variables as a backup.

namespace Poison
{
	void OnDataLoaded();
	void OnPreLoad();     // abort a running session before a save is loaded
	void OnGameLoaded();  // reset leftovers of a session saved mid-way, check for conflicting mods

	void Tick(RE::PlayerCharacter* a_player, float a_delta);  // main thread, every player update
	void PreAnimUpdate(RE::PlayerCharacter* a_player);        // before the player's graph update: clip echo for a chained repeat
	void PostAnimUpdate(RE::PlayerCharacter* a_player);       // after it: held-back events go to the graph
	bool DeferPoseExit(const RE::BSFixedString& a_event);     // NotifyAnimationGraph hook, before the original: true = held back

	// event sinks (any thread): only queue
	void OnPoisoned(RE::AlchemyItem* a_poison);
	void OnGraphEvent(const RE::BSFixedString& a_tag, const RE::BSFixedString& a_payload);  // raised by the active graph
	void OnInputEvent(const RE::BSFixedString& a_event);  // sent to the player's graph by anyone
	void PushEvent(const char* a_tag, const char* a_payload);
	bool Listening();  // a session wants graph events
}
