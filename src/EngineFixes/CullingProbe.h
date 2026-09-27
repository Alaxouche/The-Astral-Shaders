#pragma once

#include "EngineFix.h"

#include <atomic>

/**
 * @brief Go/no-go instrumentation for occlusion culling. Culls nothing.
 *
 * Detours the bodies of BSCullingProcess::Process1 / Process2 / TestBaseVisibility1 and the
 * BSParabolicCullingProcess overrides, and counts what reaches them per frame, split by whether
 * the cull runs on the main world camera. The addresses come from the vtables at runtime, so
 * this is runtime-agnostic without an id table; detouring the body rather than the vtable slot
 * also catches the devirtualized direct call the engine's main cull walk makes, which is why an
 * earlier vfunc hook on this class counted zero.
 *
 * Every 300 frames a summary goes to the log, and each detour reports the first time it fires.
 * If the main-camera Process1 count stays at zero, per-object occlusion culling has no viable
 * entry point here and no further work is warranted.
 */
struct CullingProbe : EngineFix
{
	std::string GetName() override { return "Culling Probe"; }

	void Install() override;

	struct Counters
	{
		std::atomic<std::uint64_t> baseProcess1Main{ 0 };
		std::atomic<std::uint64_t> baseProcess1Other{ 0 };
		std::atomic<std::uint64_t> baseProcess1Geometry{ 0 };
		std::atomic<std::uint64_t> baseProcess1WithRefr{ 0 };
		std::atomic<std::uint64_t> baseProcess2Main{ 0 };
		std::atomic<std::uint64_t> baseProcess2Other{ 0 };
		std::atomic<std::uint64_t> baseTestBaseVisMain{ 0 };
		std::atomic<std::uint64_t> baseTestBaseVisOther{ 0 };

		std::atomic<std::uint64_t> paraProcess1Main{ 0 };
		std::atomic<std::uint64_t> paraProcess1Other{ 0 };
		std::atomic<std::uint64_t> paraProcess1Geometry{ 0 };
		std::atomic<std::uint64_t> paraProcess1WithRefr{ 0 };
		std::atomic<std::uint64_t> paraProcess2Main{ 0 };
		std::atomic<std::uint64_t> paraProcess2Other{ 0 };
		std::atomic<std::uint64_t> paraTestBaseVisMain{ 0 };
		std::atomic<std::uint64_t> paraTestBaseVisOther{ 0 };
	};

	static inline Counters counters;
	static inline std::atomic<std::uint32_t> lastReportFrame{ 0 };
	static constexpr std::uint32_t kReportInterval = 300;

	/** @brief Logs and resets the counters once per kReportInterval frames; safe from any thunk. */
	static void MaybeReport();

private:
	static bool IsMainCamera(const RE::NiCamera* a_camera);

	struct Base_Process1
	{
		static void thunk(RE::BSCullingProcess* a_this, RE::NiAVObject* a_object, std::int32_t a_arg2);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct Base_Process2
	{
		static void thunk(RE::BSCullingProcess* a_this, const RE::NiCamera* a_camera, RE::NiAVObject* a_scene, RE::NiVisibleArray* a_visibleSet);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct Base_TestBaseVisibility1
	{
		static bool thunk(RE::BSCullingProcess* a_this, RE::BSMultiBound& a_bound);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct Para_Process1
	{
		static void thunk(RE::BSParabolicCullingProcess* a_this, RE::NiAVObject* a_object, std::int32_t a_arg2);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct Para_Process2
	{
		static void thunk(RE::BSParabolicCullingProcess* a_this, const RE::NiCamera* a_camera, RE::NiAVObject* a_scene, RE::NiVisibleArray* a_visibleSet);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct Para_TestBaseVisibility1
	{
		static bool thunk(RE::BSParabolicCullingProcess* a_this, RE::BSMultiBound& a_bound);
		static inline REL::Relocation<decltype(thunk)> func;
	};
};
