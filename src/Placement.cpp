#include "Placement.h"

#include "Settings.h"

namespace Placement
{
	namespace
	{
		struct Rule
		{
			int                                dogUp;
			std::array<const char*, 8>         subs;    // unused tail = nullptr
			bool                               always;  // author's condition without "!= -1": matches practically anything
			std::array<float, 3>               pos;
			std::array<float, 3>               rot;
			float                              scale;
		};

		const std::vector<Rule>& Rules()
		{
			static const std::vector<Rule> rules{
#include "BuiltinTransforms.inl"
			};
			return rules;
		}

		std::string Lower(std::string_view a_s)
		{
			std::string r(a_s);
			std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return r;
		}

		// StringUtil.Find(a, b) != -1 (SKSE: case-insensitive)
		bool Contains(std::string_view a_hay, std::string_view a_needle)
		{
			return Lower(a_hay).find(Lower(a_needle)) != std::string::npos;
		}

		// the author splits the model path on '\' and takes the last part
		std::string NifName(std::string_view a_modelPath)
		{
			const auto pos = a_modelPath.rfind('\\');
			return Lower(pos == std::string_view::npos ? a_modelPath : a_modelPath.substr(pos + 1));
		}

		// --- JSON files: {"floatList": {"<nif name>": [pos xyz, rot xyz, scale]}}, keys case-insensitive ------------

		struct JsonFile
		{
			std::filesystem::file_time_type                           mtime{};
			bool                                                      loaded{ false };
			bool                                                      good{ false };
			std::unordered_map<std::string, std::vector<float>>       floats;
		};

		const std::filesystem::path kRoot{ "Data/SKSE/Plugins" };

		constexpr std::array kThirdPerson{ "AnimatedPoisonsRedone/ThirdPerson" };
		constexpr std::array kFirstPerson{ "AnimatedPoisonsRedone/FirstPerson" };

		std::mutex                                       g_lock;
		std::unordered_map<std::string, JsonFile>        g_files;

		const JsonFile* Load(const std::filesystem::path& a_path)
		{
			std::error_code ec;
			const auto      mtime = std::filesystem::last_write_time(a_path, ec);
			if (ec) {
				return nullptr;
			}
			auto& f = g_files[Lower(a_path.generic_string())];
			if (f.loaded && f.mtime == mtime) {
				return f.good ? &f : nullptr;
			}
			f = JsonFile{};
			f.mtime = mtime;
			f.loaded = true;
			try {
				std::ifstream in(a_path);
				const auto    j = json::parse(in, nullptr, true, true);  // comments of hand-edited files tolerated
				if (auto it = j.find("floatList"); it != j.end() && it->is_object()) {
					for (auto& [k, v] : it->items()) {
						auto& out = f.floats[Lower(k)];
						for (auto& x : v) {
							out.push_back(x.is_number() ? x.get<float>() : 0.0f);
						}
					}
				}
				f.good = true;
				if (Settings::Get().debugLog) {
					logger::info("json {}: {} entries", a_path.generic_string(), f.floats.size());
				}
			} catch (const std::exception& e) {
				logger::warn("json {}: {}", a_path.generic_string(), e.what());
			}
			return f.good ? &f : nullptr;
		}

		// the author's search: extra files whose name contains a_list (not <a_list>.json itself) that have the key,
		// last in the folder listing first; then <a_list>.json
		template <class Has>
		const JsonFile* Find(const char* a_folder, std::string_view a_list, Has&& a_has)
		{
			const auto            dir = kRoot / a_folder;
			std::vector<std::string> names;
			std::error_code       ec;
			for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
				if (e.is_regular_file(ec) && Lower(e.path().extension().string()) == ".json") {
					names.push_back(e.path().filename().string());
				}
			}
			std::sort(names.begin(), names.end(), [](auto& a, auto& b) { return Lower(a) < Lower(b); });
			const auto def = Lower(std::string(a_list) + ".json");
			if (names.size() > 1) {
				for (auto it = names.rbegin(); it != names.rend(); ++it) {
					if (Contains(*it, a_list) && Lower(*it) != def) {
						if (auto f = Load(dir / *it); f && a_has(*f)) {
							return f;
						}
					}
				}
			}
			auto f = Load(dir / (std::string(a_list) + ".json"));
			return f && a_has(*f) ? f : nullptr;
		}

		// ReadTransformFromJson(a_list, a_modelPath)
		bool ReadTransform(std::string_view a_list, std::string_view a_modelPath, Transform& a_out, bool a_firstPerson = false)
		{
			const auto     nif = NifName(a_modelPath);
			std::lock_guard lock(g_lock);
			const JsonFile* f = nullptr;
			for (auto folder : a_firstPerson ? kFirstPerson : kThirdPerson) {
				if ((f = Find(folder, a_list, [&](const JsonFile& a_f) {
						auto it = a_f.floats.find(nif);
						return it != a_f.floats.end() && !it->second.empty();
					}))) {
					break;
				}
			}
			if (!f) {
				return false;
			}
			const auto& v = f->floats.at(nif);
			auto        at = [&](std::size_t i) { return i < v.size() ? v[i] : 0.0f; };
			a_out.pos = { at(0), at(1), at(2) };
			a_out.rot = { at(3), at(4), at(5) };
			a_out.scale = at(6);
			a_out.source = a_firstPerson ? "json 1P" : "json";
			return true;
		}

		bool ApplyRules(int a_dogUp, const std::string& a_modelPath, Transform& a_out)
		{
			for (auto& r : Rules()) {
				if (r.dogUp != a_dogUp) {
					continue;
				}
				bool hit = r.always;
				for (auto s : r.subs) {
					hit = hit || (s && Contains(a_modelPath, s));
				}
				if (hit) {
					a_out.pos = r.pos;
					a_out.rot = r.rot;
					a_out.scale = r.scale;
					a_out.source = "built-in";
					return true;
				}
			}
			return false;
		}

		void Set(Transform& a_t, std::array<float, 3> a_pos, std::array<float, 3> a_rot, float a_scale = 1.0f)
		{
			a_t.pos = a_pos;
			a_t.rot = a_rot;
			a_t.scale = a_scale;
			a_t.source = "built-in";
		}
	}

	std::string ModelPath(const RE::TESForm* a_form)
	{
		const auto model = a_form ? a_form->As<RE::TESModel>() : nullptr;
		const auto path = model ? model->GetModel() : nullptr;
		return path ? path : "";
	}

	Transform ForDisplay(const RE::TESForm* a_form)
	{
		// our JSON first for every item -- the bottle, the arrow / bolt projectile and the FX (PoisonCloak01.nif), keyed by
		// the nif name in PoisonList*.json; then the built-in values (bottles and FX: the original mod's)
		Transform  t;
		const auto model = ModelPath(a_form);
		if (ReadTransform("PoisonList", model, t)) {
			return t;
		}
		if (a_form && a_form->GetFormType() == RE::FormType::Projectile) {
			// the original mod's arrow; any arrow / bolt is fitted along its shaft (the hand at 55 % from the tip, as on that
			// arrow -- a vanilla bolt gives -4.8 10.6 0; mod models with their own origin too)
			Set(t, { -14.5f, 28.5f, 0.0f }, { 0.0f, 28.5f, 0.0f });
			t.shaftFit = 0.55f;
			return t;
		}
		if (Contains(model, "poisoncloak01")) {  // the FX
			Set(t, { 0.0f, 30.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, 0.35f);
			return t;
		}
		if (!ApplyRules(60, model, t)) {
			Set(t, { -1.5f, -11.0f, 5.0f }, { 13.5f, 24.5f, -105.0f });
		}
		return t;
	}

	Transform ForDisplay1P(const RE::TESForm* a_form)
	{
		// measured from the original mod's 1P nifs: those nifs are vanilla geometry moved under the node, so
		// P = (shape transform in his 1P nif) * (the same shape in the vanilla nif)^-1 -- given per nif name in
		// FirstPerson/PoisonList / ArrowList floatList (79 bottles on AnimObjectL, 16 arrow projectiles on NPC R Hand [RHnd]).
		// Not listed (mod poisons, custom arrows): the same values by bottle class / arrow, measured there
		Transform  t;
		const auto model = ModelPath(a_form);
		const bool projectile = a_form && a_form->GetFormType() == RE::FormType::Projectile;
		if (ReadTransform(projectile ? "ArrowList" : "PoisonList", model, t, true)) {
			return t;
		}
		if (projectile) {
			// GiraPomba's 1P arrow as the engine hangs it: the AnimObject loader (SE 15526) resets the clone root's
			// local transform to identity and 15569 parents it to the Prn node (NPC R Hand [RHnd]) -- his root rotation is
			// never used. So P = his shape's local * the projectile shape's local^-1, NIF matrices as stored (68888 reads the 9
			// floats in order, NiTransform::operator* 19922 multiplies rows by the column vector): the shaft through the fist,
			// the tip along the hand's +X. Any arrow / bolt is fitted along its shaft (the hand at 73 % from the tip, as on the
			// steel arrow -- a vanilla bolt gives 13.0 -2 10.6)
			Set(t, { 39.0f, -2.0f, 18.0f }, { -90.0f, -74.0f, 90.0f });
			t.shaftFit = 0.73f;
		} else if (Contains(model, "frostbitevenom01")) {
			Set(t, { -2.0f, -6.5f, -0.6f }, { 0.0f, 0.0f, -90.0f }, 0.8f);
		} else if (Contains(model, "weakness")) {
			Set(t, { -2.0f, -6.0f, -1.0f }, { 0.0f, 0.0f, -90.0f });
		} else if (Contains(model, "damageskill") || Contains(model, "damagearmor") || Contains(model, "damageweapon")) {
			Set(t, { -2.0f, -8.0f, -0.5f }, { 0.0f, 0.0f, -90.0f });  // class 02
		} else if (Contains(model, "linger01") || Contains(model, "01.nif")) {
			Set(t, { -2.0f, -7.0f, -1.0f }, { 0.0f, 0.0f, -90.0f });  // class 01
		} else if (Contains(model, "03.nif") || Contains(model, "04.nif") || Contains(model, "05.nif")) {
			Set(t, { -2.0f, -9.0f, 3.5f }, { 0.0f, 0.0f, -109.0f });  // class 03 / 04 / 05
		} else {
			Set(t, { -2.0f, -8.0f, -0.5f }, { 0.0f, 0.0f, -90.0f });  // class 02, the default
		}
		t.source = "built-in 1P";
		return t;
	}
}
