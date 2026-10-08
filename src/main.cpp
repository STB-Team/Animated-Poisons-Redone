// Animated Poisons Redone -- the poison-applying animation of Immersive Interactions - New Anims
// through Offset Movement Animation as a standalone SKSE plugin: no Immersive Interactions, no Papyrus.
// One DLL for SE 1.5.97, AE 1.6.x and 1.7.x (CommonLibSSE-NG + Address Library, addresses via RELOCATION_ID).
// See README.md / AGENTS.md.

#include "Diagnostics.h"
#include "Hooks.h"
#include "Poison.h"
#include "Settings.h"

static void SKSEMessageHandler(SKSE::MessagingInterface::Message* a_message)
{
	switch (a_message->type) {
	case SKSE::MessagingInterface::kDataLoaded:
		Diagnostics::OnDataLoaded();
		Poison::OnDataLoaded();
		Hooks::Install();
		break;
	case SKSE::MessagingInterface::kPreLoadGame:
		Poison::OnPreLoad();
		break;
	case SKSE::MessagingInterface::kPostLoadGame:
	case SKSE::MessagingInterface::kNewGame:
		Hooks::OnGameLoaded();
		Poison::OnGameLoaded();
		break;
	default:
		break;
	}
}

// SKSEPlugin_Version (AE/1.7 SKSE) + SKSEPlugin_Query (SE SKSE). Address Library + no struct use: SKSE loads
// the DLL on every runtime, NG picks the SE or AE id of each RELOCATION_ID at startup.
SKSEPluginInfo(
	.Version = REL::Version{ Version::MAJOR, Version::MINOR, Version::PATCH, 0 },
	.Name = Version::PROJECT,
	.Author = "STB Team"sv,
	.StructCompatibility = SKSE::StructCompatibility::Independent,
	.RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

static void InitLog()
{
	auto path = logger::log_directory();
	if (!path) {
		return;
	}

	*path /= Version::PROJECT;
	*path += ".log"sv;
	{  // keep the previous run (e.g. before a crash): X.log -> X.1.log
		auto            prev = path->parent_path() / (std::string(Version::PROJECT) + ".1.log");
		std::error_code ec;
		if (std::filesystem::exists(*path, ec)) {
			std::filesystem::remove(prev, ec);
			std::filesystem::rename(*path, prev, ec);
		}
	}
	auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
	auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));

	log->set_level(spdlog::level::info);
	log->flush_on(spdlog::level::info);

	spdlog::set_default_logger(std::move(log));
	spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v"s);
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	InitLog();
	SKSE::Init(a_skse);
	logger::info("{} v{}", Version::PROJECT, Version::NAME);
	Settings::Get().Load();
	Diagnostics::OnLoad(a_skse);
	Hooks::InstallEarly();

	auto messaging = SKSE::GetMessagingInterface();
	if (!messaging || !messaging->RegisterListener("SKSE", SKSEMessageHandler)) {
		logger::critical("Failed to register messaging listener"sv);
		return false;
	}

	logger::info("loaded"sv);
	return true;
}
