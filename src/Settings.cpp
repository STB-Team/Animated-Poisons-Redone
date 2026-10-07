#include "Settings.h"

#include <SimpleIni.h>

namespace
{
	// "a, b; c d" -> {"a", "b", "c", "d"}
	std::vector<std::string> SplitList(std::string_view a_list)
	{
		std::vector<std::string> out;
		std::string              cur;
		for (const char c : a_list) {
			if (c == ',' || c == ';' || c == ' ' || c == '\t') {
				if (!cur.empty()) {
					out.push_back(std::move(cur));
					cur.clear();
				}
			} else {
				cur.push_back(c);
			}
		}
		if (!cur.empty()) {
			out.push_back(std::move(cur));
		}
		return out;
	}

	std::string JoinList(const std::vector<std::string>& a_list)
	{
		std::string out;
		for (auto& s : a_list) {
			out += out.empty() ? "" : " ";
			out += s;
		}
		return out.empty() ? "-" : out;
	}
}

void Settings::Load()
{
	CSimpleIniA ini;
	ini.SetUnicode();
	const auto path = "Data/SKSE/Plugins/AnimatedPoisonsRedone.ini";
	if (ini.LoadFile(path) < 0) {
		logger::warn("{} not found, defaults", path);
	} else {
		// only [Settings]; keys of earlier versions ([General] / [Poison] / [Interrupt] / [Debug]) are ignored
		animSpeed = static_cast<float>(ini.GetDoubleValue("Settings", "fAnimSpeed", animSpeed));
		repeatWindow = static_cast<float>(ini.GetDoubleValue("Settings", "fRepeatWindow", repeatWindow));
		drawSheathed = ini.GetBoolValue("Settings", "bDrawSheathed", drawSheathed);
		crossbow = ini.GetBoolValue("Settings", "bCrossbow", crossbow);
		playInMenus = ini.GetBoolValue("Settings", "bPlayInMenus", playInMenus);
		debugLog = ini.GetBoolValue("Settings", "bDebugLog", debugLog);
	}
	interruptEvents = SplitList(kInterruptEvents);
	poseExitEvents = SplitList(kPoseExitEvents);
	interruptVariables = SplitList(kInterruptVariables);
	animSpeed = std::clamp(animSpeed, 0.5f, 3.0f);
	repeatWindow = std::clamp(repeatWindow, 0.0f, 0.9f);

	// what is really in effect -- tells an old deployed INI from the current one
	logger::info("settings: speed {:g}, repeat window {:g}, draw sheathed {}, crossbow {}, play in menus {}, debug log {}", animSpeed,
		repeatWindow, drawSheathed, crossbow, playInMenus, debugLog);
	if (debugLog) {
		logger::info("interrupt: events {}; variables {}", JoinList(interruptEvents), JoinList(interruptVariables));
	}
}
