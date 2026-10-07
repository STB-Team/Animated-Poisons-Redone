#include "HandItems.h"

#include "Settings.h"

#include <numbers>

namespace HandItems
{
	namespace
	{
		// engine functions that attach a model to an actor's skeleton (SE id, AE id)
		bool AttachAddonNodes(RE::NiNode* a_node)
		{
			using func_t = bool (*)(RE::NiNode*);
			static REL::Relocation<func_t> func{ RELOCATION_ID(19206, 19632) };
			return func(a_node);
		}

		void AttachAddonParticles(RE::NiAVObject* a_object)
		{
			using func_t = void (*)(RE::NiAVObject*);
			static REL::Relocation<func_t> func{ RELOCATION_ID(19207, 19633) };
			func(a_object);
		}

		void SetShaderPropsFadeNode(RE::NiAVObject* a_object, RE::BSFadeNode* a_root)
		{
			using func_t = RE::NiNode* (*)(RE::NiAVObject*, RE::BSFadeNode*);
			static REL::Relocation<func_t> func{ RELOCATION_ID(98895, 105542) };
			func(a_object, a_root);
		}

		// the object's lights / geometry into the shadow scene
		void ShadowSceneAdd(RE::NiAVObject* a_object)
		{
			using func_t = RE::NiNode* (*)(RE::ShadowSceneNode*, RE::NiAVObject*);
			static REL::Relocation<func_t> a{ RELOCATION_ID(99702, 106336) };
			static REL::Relocation<func_t> b{ RELOCATION_ID(99696, 106330) };
			const auto                     ssn = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
			if (ssn) {
				a(ssn, a_object);
				b(ssn, a_object);
			}
		}

		void StripCollision(RE::NiAVObject* a_object)
		{
			using func_t = bool (*)(RE::NiAVObject*, bool, bool);
			static REL::Relocation<func_t> func{ RELOCATION_ID(76037, 77870) };
			func(a_object, true, true);
		}

		void CleanupObject(const RE::ObjectRefHandle& a_handle, RE::NiAVObject* a_object)
		{
			using func_t = void (*)(const RE::ObjectRefHandle&, RE::NiAVObject*);
			static REL::Relocation<func_t> func{ RELOCATION_ID(15495, 15660) };
			func(a_handle, a_object);
		}

		// extrinsic rotation: R = Rx(rz) * Rz(ry) * Ry(rx), degrees, v' = R * v
		RE::NiMatrix3 EulerExtrinsic(const std::array<float, 3>& a_deg)
		{
			const auto rad = [](float d) { return d * std::numbers::pi_v<float> / 180.0f; };
			const float x = rad(a_deg[0]), y = rad(a_deg[1]), z = rad(a_deg[2]);
			auto mul = [](const RE::NiMatrix3& a, const RE::NiMatrix3& b) {
				RE::NiMatrix3 r;
				for (int i = 0; i < 3; ++i) {
					for (int j = 0; j < 3; ++j) {
						r.entry[i][j] = a.entry[i][0] * b.entry[0][j] + a.entry[i][1] * b.entry[1][j] + a.entry[i][2] * b.entry[2][j];
					}
				}
				return r;
			};
			auto rx = [](float a) {
				RE::NiMatrix3 m;
				m.entry[0][0] = 1; m.entry[0][1] = 0; m.entry[0][2] = 0;
				m.entry[1][0] = 0; m.entry[1][1] = std::cos(a); m.entry[1][2] = -std::sin(a);
				m.entry[2][0] = 0; m.entry[2][1] = std::sin(a); m.entry[2][2] = std::cos(a);
				return m;
			};
			auto ry = [](float a) {
				RE::NiMatrix3 m;
				m.entry[0][0] = std::cos(a); m.entry[0][1] = 0; m.entry[0][2] = std::sin(a);
				m.entry[1][0] = 0; m.entry[1][1] = 1; m.entry[1][2] = 0;
				m.entry[2][0] = -std::sin(a); m.entry[2][1] = 0; m.entry[2][2] = std::cos(a);
				return m;
			};
			auto rz = [](float a) {
				RE::NiMatrix3 m;
				m.entry[0][0] = std::cos(a); m.entry[0][1] = -std::sin(a); m.entry[0][2] = 0;
				m.entry[1][0] = std::sin(a); m.entry[1][1] = std::cos(a); m.entry[1][2] = 0;
				m.entry[2][0] = 0; m.entry[2][1] = 0; m.entry[2][2] = 1;
				return m;
			};
			return mul(mul(rx(z), rz(y)), ry(x));
		}

		// the shaft of a projectile model = its geometry with the most vertices (not under TracerRoot: the flight trail),
		// its bound sphere in the model root's space (the root's own transform is ours): along the model's y the tip =
		// centre.y + radius, the length = the diameter. Vanilla: tip 0, 58.2 (steel arrow) / 21.2 (bolt); Requiem's bolts
		// have their own origin and scale (tip ~ +27.6)
		struct Shaft
		{
			std::uint16_t vertices{ 0 };
			float         tip{ 0.0f };
			float         length{ 0.0f };
		};

		void FindShaft(RE::NiAVObject* a_object, const RE::NiTransform& a_toRoot, Shaft& a_best)
		{
			if (!a_object || (a_object->name.c_str() && _stricmp(a_object->name.c_str(), "TracerRoot") == 0)) {
				return;
			}
			const auto here = a_toRoot * a_object->local;
			if (const auto shape = a_object->AsTriShape()) {
				const auto count = shape->GetTrishapeRuntimeData().vertexCount;
				if (count > a_best.vertices) {
					const auto& bound = shape->GetModelData().modelBound;
					const auto  centre = here * bound.center;
					a_best = { count, centre.y + bound.radius * here.scale, 2.0f * bound.radius * here.scale };
				}
			}
			if (const auto node = a_object->AsNode()) {
				for (auto& child : node->GetChildren()) {
					FindShaft(child.get(), here, a_best);
				}
			}
		}

		// the hand at a_share of the shaft from the tip, as on the reference arrow (tip 0, kRefShaft): moved along the
		// model's own y axis, in the hand node's space through the item's rotation
		void FitShaft(RE::NiAVObject* a_object, float a_share, const char* a_name)
		{
			Shaft      best;
			RE::NiTransform root;  // identity: the model root's space
			if (const auto node = a_object->AsNode()) {
				for (auto& child : node->GetChildren()) {
					FindShaft(child.get(), root, best);
				}
			}
			if (best.vertices == 0 || best.length <= 0.0f) {
				logger::warn("hand item {}: no shaft geometry found, placed as the reference arrow", a_name);
				return;
			}
			const float shift = -a_share * Placement::kRefShaft - (best.tip - a_share * best.length);
			const auto& r = a_object->local.rotate;
			const float s = a_object->local.scale;
			a_object->local.translate += RE::NiPoint3(r.entry[0][1], r.entry[1][1], r.entry[2][1]) * (shift * s);
			if (!Settings::Get().debugLog) {
				return;
			}
			logger::info("hand item {}: shaft tip {:.1f}, length {:.1f} ({} vertices) -> moved {:.1f} along it, pos {:.2f} {:.2f} {:.2f}",
				a_name, best.tip, best.length, best.vertices, shift, a_object->local.translate.x, a_object->local.translate.y,
				a_object->local.translate.z);
		}

		RE::NiPointer<RE::NiNode> Load(const std::string& a_model)
		{
			// TESModel paths are relative to Data/meshes; the model DB takes them with the folder
			std::string path = a_model;
			if (_strnicmp(path.c_str(), "meshes\\", 7) != 0 && _strnicmp(path.c_str(), "meshes/", 7) != 0) {
				path = "meshes\\" + path;
			}
			RE::NiPointer<RE::NiNode>              model;
			const RE::BSModelDB::DBTraits::ArgsType args{};
			const auto                             err = RE::BSModelDB::Demand(path.c_str(), model, args);
			if (err != RE::BSResource::ErrorCode::kNone || !model) {
				logger::error("hand item model not loaded: {} (error {})", path, static_cast<int>(err));
				return nullptr;
			}
			return model;
		}

		struct Item
		{
			std::string                   name;
			RE::NiPointer<RE::NiAVObject> object;
			RE::ObjectRefHandle           owner;
		};
		std::vector<Item> g_items;
		std::vector<std::pair<Item, int>> g_retiring;  // removed when the count reaches 0 (frames)

		void Remove(Item& a_item)
		{
			if (!a_item.object) {
				return;
			}
			if (const auto parent = a_item.object->parent) {  // no parent: the actor's 3D was rebuilt, the item went with it
				CleanupObject(a_item.owner, a_item.object.get());
				parent->DetachChild2(a_item.object.get());
			}
			a_item.object.reset();
		}
	}

	bool Create(RE::Actor* a_actor, const char* a_name, const std::string& a_model, const char* a_node, const Placement::Transform& a_t,
		bool a_projectile, bool a_firstPerson)
	{
		auto it = std::find_if(g_items.begin(), g_items.end(), [&](auto& e) { return e.name == a_name; });
		if (it != g_items.end()) {
			Remove(*it);
			g_items.erase(it);
		}
		if (!a_actor || a_model.empty()) {
			logger::warn("hand item {}: no actor / no model", a_name);
			return false;
		}
		const auto root3D = a_actor->Get3D(a_firstPerson);
		const auto root = root3D ? root3D->AsNode() : nullptr;
		const auto target = root ? root->GetObjectByName(a_node) : nullptr;
		const auto targetNode = target ? target->AsNode() : nullptr;
		if (!targetNode) {
			logger::error("hand item {}: node {} not found on the {} skeleton", a_name, a_node, a_firstPerson ? "1st-person" : "3rd-person");
			return false;
		}
		const auto loaded = Load(a_model);
		if (!loaded) {
			return false;
		}
		RE::NiPointer<RE::NiAVObject> object(loaded->Clone());
		if (!object) {
			logger::error("hand item {}: model {} not cloned", a_name, a_model);
			return false;
		}

		const auto fadeRoot = root->AsFadeNode();
		if (const auto node = object->AsNode()) {
			if (const auto bsx = object->GetExtraData<RE::BSXFlags>("BSX"); bsx && (bsx->value & static_cast<std::int32_t>(RE::BSXFlags::Flag::kAddon))) {
				AttachAddonNodes(node);
			}
		}
		AttachAddonParticles(object.get());
		if (const auto fade = object->AsFadeNode()) {  // no LOD fade for an item in the hand
			auto& unk153 = fade->GetRuntimeData().unk153;
			unk153 = static_cast<std::uint8_t>((unk153 & 0xF0) | 0x7);
		}
		if (fadeRoot) {
			SetShaderPropsFadeNode(object.get(), fadeRoot);
		}

		object->local.translate = RE::NiPoint3(a_t.pos[0], a_t.pos[1], a_t.pos[2]);
		object->local.rotate = EulerExtrinsic(a_t.rot);
		if (a_t.shaftFit > 0.0f) {
			FitShaft(object.get(), a_t.shaftFit, a_name);
		}
		object->local.scale = a_t.scale;
		object->name = std::string("APR_") + a_name;
		object->SetAppCulled(true);  // shown by Show()

		targetNode->AttachChild(object.get(), true);
		RE::NiUpdateData update{};
		object->UpdateDownwardPass(update, 0);
		ShadowSceneAdd(object.get());

		if (a_projectile) {  // the projectile's flight trail
			if (const auto tracer = object->GetObjectByName("TracerRoot"); tracer && tracer->parent) {
				tracer->parent->DetachChild2(tracer);
			}
		}
		StripCollision(object.get());
		if (fadeRoot) {
			SetShaderPropsFadeNode(targetNode, fadeRoot);
		}

		g_items.push_back({ a_name, object, a_actor->GetHandle() });
		return true;
	}

	void Show(const char* a_name, bool a_on)
	{
		for (auto& e : g_items) {
			if (e.name == a_name && e.object) {
				e.object->SetAppCulled(!a_on);
			}
		}
	}

	void RemoveAll()
	{
		for (auto& e : g_items) {
			Remove(e);
		}
		g_items.clear();
		for (auto& [e, frames] : g_retiring) {
			Remove(e);
		}
		g_retiring.clear();
	}

	void RetireAll(int a_frames)
	{
		for (auto& e : g_items) {
			g_retiring.emplace_back(std::move(e), a_frames);
		}
		g_items.clear();
	}

	void Tick()
	{
		for (auto it = g_retiring.begin(); it != g_retiring.end();) {
			if (--it->second <= 0) {
				Remove(it->first);
				it = g_retiring.erase(it);
			} else {
				++it;
			}
		}
	}
}
