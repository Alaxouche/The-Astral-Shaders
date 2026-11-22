// Inspired by "Hitch Hunter and Fixes - SKSE" by Zzyxzz: https://www.nexusmods.com/skyrimspecialedition/mods/194231
#include "SunOcclusionPollFix.h"

#include "Utils/VersionedRelocation.h"

namespace
{
	// Poll arguments as encoded on SE, AE 1.6 and AE 1.7: r9b = flush flag, rcx = renderer data, r8d = wait flag.
	constexpr auto PollArguments = REL::make_pattern<"41 B1 01 48 8D 0D ?? ?? ?? ?? 45 33 C0">();
	constexpr std::uintptr_t PollArgumentsSize = 13;
	constexpr std::uintptr_t FlushFlagOffset = 2;

	std::uintptr_t GetPollCall()
	{
		return REL::RelocationID(99967, 106605).address() + REL::Relocate(0x42, 0x85);
	}
}

// Hitch Hunter (SunQueryNoFlush) hooks this call and clears the flush flag itself. With that setting off the
// call is untouched and the fix installs normally.
const char* SunOcclusionPollFix::GetConflictingPlugin() const
{
	if (GetModuleHandleW(L"HitchHunter.dll") && Util::VersionedRelocation::IsCallHookedByPlugin(GetPollCall()))
		return "Hitch Hunter";
	return nullptr;
}

void SunOcclusionPollFix::Install()
{
	const auto pollCall = GetPollCall();
	const auto pollArguments = pollCall - PollArgumentsSize;
	if (!REL::verify_code(pollArguments, PollArguments) ||
		!Util::VersionedRelocation::IsCallTo(pollCall, REL::RelocationID(75569, 77370).address())) {
		logger::warn("[Engine Fixes] {} not applied: unexpected code at the sun occlusion poll (another mod or an unverified runtime)", GetName());
		return;
	}
	REL::safe_write(pollArguments + FlushFlagOffset, std::uint8_t{ 0 });
}
