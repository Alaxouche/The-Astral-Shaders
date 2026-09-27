#include "CullingProbe.h"

#include "Globals.h"

#include <RE/B/BSCullingProcess.h>
#include <RE/B/BSGeometry.h>
#include <RE/B/BSMultiBound.h>
#include <RE/B/BSParabolicCullingProcess.h>
#include <RE/M/Main.h>
#include <RE/N/NiCamera.h>

namespace
{
	// Vtable indices as documented in CommonLibSSE-NG's NiCullingProcess / BSCullingProcess.
	constexpr std::size_t kSlotProcess1 = 0x16;
	constexpr std::size_t kSlotProcess2 = 0x17;
	constexpr std::size_t kSlotTestBaseVisibility1 = 0x1A;

	std::uintptr_t ReadSlot(REL::VariantID a_vtable, std::size_t a_slot)
	{
		REL::Relocation<std::uintptr_t*> vtbl{ a_vtable };
		return vtbl.get()[a_slot];
	}

	template <class T>
	bool Attach(const char* a_name, std::uintptr_t a_address)
	{
		const auto rva = a_address - REL::Module::get().base();
		stl::detour_thunk<T>(a_address);
		// DetourAttach rewrites func to the trampoline on success; unchanged means it failed.
		const bool ok = T::func.address() != a_address;
		logger::info("[Culling Probe] {} body at RVA 0x{:X}: detour {}", a_name, rva, ok ? "attached" : "FAILED");
		return ok;
	}

	template <class Flag>
	void ReportFirstHit(Flag& a_flag, const char* a_name)
	{
		bool expected = false;
		if (a_flag.compare_exchange_strong(expected, true, std::memory_order_relaxed))
			logger::info("[Culling Probe] first hit: {}", a_name);
	}

	std::atomic<bool> hitBaseP1{ false }, hitBaseP2{ false }, hitBaseTBV{ false };
	std::atomic<bool> hitParaP1{ false }, hitParaP2{ false }, hitParaTBV{ false };

	void CountProcess1(RE::NiAVObject* a_object, bool a_main,
		std::atomic<std::uint64_t>& a_mainCounter, std::atomic<std::uint64_t>& a_otherCounter,
		std::atomic<std::uint64_t>& a_geometry, std::atomic<std::uint64_t>& a_withRefr)
	{
		(a_main ? a_mainCounter : a_otherCounter).fetch_add(1, std::memory_order_relaxed);
		if (!a_main || !a_object)
			return;
		if (netimmerse_cast<RE::BSGeometry*>(a_object))
			a_geometry.fetch_add(1, std::memory_order_relaxed);
		if (a_object->GetUserData())
			a_withRefr.fetch_add(1, std::memory_order_relaxed);
	}
}

void CullingProbe::Install()
{
	const auto baseP1 = ReadSlot(RE::VTABLE_BSCullingProcess[0], kSlotProcess1);
	const auto baseP2 = ReadSlot(RE::VTABLE_BSCullingProcess[0], kSlotProcess2);
	const auto baseTBV = ReadSlot(RE::VTABLE_BSCullingProcess[0], kSlotTestBaseVisibility1);
	const auto paraP1 = ReadSlot(RE::VTABLE_BSParabolicCullingProcess[0], kSlotProcess1);
	const auto paraP2 = ReadSlot(RE::VTABLE_BSParabolicCullingProcess[0], kSlotProcess2);
	const auto paraTBV = ReadSlot(RE::VTABLE_BSParabolicCullingProcess[0], kSlotTestBaseVisibility1);

	// The parabolic class overrides all three; if a slot resolves to the base body the override
	// is absent on this runtime and detouring it twice would double-count.
	if (paraP1 == baseP1 || paraP2 == baseP2 || paraTBV == baseTBV) {
		logger::warn("[Culling Probe] BSParabolicCullingProcess shares a body with BSCullingProcess; skipping the shared slot(s)");
	}

	Attach<Base_Process1>("BSCullingProcess::Process1", baseP1);
	Attach<Base_Process2>("BSCullingProcess::Process2", baseP2);
	Attach<Base_TestBaseVisibility1>("BSCullingProcess::TestBaseVisibility1", baseTBV);
	if (paraP1 != baseP1)
		Attach<Para_Process1>("BSParabolicCullingProcess::Process1", paraP1);
	if (paraP2 != baseP2)
		Attach<Para_Process2>("BSParabolicCullingProcess::Process2", paraP2);
	if (paraTBV != baseTBV)
		Attach<Para_TestBaseVisibility1>("BSParabolicCullingProcess::TestBaseVisibility1", paraTBV);

	logger::info("[Culling Probe] installed; culls nothing, reports every {} frames", kReportInterval);
}

bool CullingProbe::IsMainCamera(const RE::NiCamera* a_camera)
{
	return a_camera && a_camera == RE::Main::WorldRootCamera();
}

void CullingProbe::MaybeReport()
{
	auto* graphicsState = globals::game::graphicsState;
	if (!graphicsState)
		return;
	const auto frame = graphicsState->frameCount;
	auto last = lastReportFrame.load(std::memory_order_relaxed);
	if (frame - last < kReportInterval)
		return;
	// One thunk wins the report for this window; the others see the updated frame and return.
	if (!lastReportFrame.compare_exchange_strong(last, frame, std::memory_order_relaxed))
		return;

	auto& c = counters;
	const auto take = [](std::atomic<std::uint64_t>& a_counter) { return a_counter.exchange(0, std::memory_order_relaxed); };
	const double window = static_cast<double>(kReportInterval);

	const auto bP1m = take(c.baseProcess1Main), bP1o = take(c.baseProcess1Other);
	const auto bP1g = take(c.baseProcess1Geometry), bP1r = take(c.baseProcess1WithRefr);
	const auto bP2m = take(c.baseProcess2Main), bP2o = take(c.baseProcess2Other);
	const auto bTm = take(c.baseTestBaseVisMain), bTo = take(c.baseTestBaseVisOther);
	const auto pP1m = take(c.paraProcess1Main), pP1o = take(c.paraProcess1Other);
	const auto pP1g = take(c.paraProcess1Geometry), pP1r = take(c.paraProcess1WithRefr);
	const auto pP2m = take(c.paraProcess2Main), pP2o = take(c.paraProcess2Other);
	const auto pTm = take(c.paraTestBaseVisMain), pTo = take(c.paraTestBaseVisOther);

	logger::info("[Culling Probe] frame {} | per-frame averages over {} frames, main camera / other cameras",
		frame, kReportInterval);
	logger::info("[Culling Probe]   BSCullingProcess          Process1 {:.1f}/{:.1f} (main: geometry {:.1f}, with refr {:.1f}) | Process2 {:.2f}/{:.2f} | TestBaseVisibility1 {:.1f}/{:.1f}",
		bP1m / window, bP1o / window, bP1g / window, bP1r / window, bP2m / window, bP2o / window, bTm / window, bTo / window);
	logger::info("[Culling Probe]   BSParabolicCullingProcess Process1 {:.1f}/{:.1f} (main: geometry {:.1f}, with refr {:.1f}) | Process2 {:.2f}/{:.2f} | TestBaseVisibility1 {:.1f}/{:.1f}",
		pP1m / window, pP1o / window, pP1g / window, pP1r / window, pP2m / window, pP2o / window, pTm / window, pTo / window);
}

void CullingProbe::Base_Process1::thunk(RE::BSCullingProcess* a_this, RE::NiAVObject* a_object, std::int32_t a_arg2)
{
	ReportFirstHit(hitBaseP1, "BSCullingProcess::Process1");
	CountProcess1(a_object, IsMainCamera(a_this->camera),
		counters.baseProcess1Main, counters.baseProcess1Other, counters.baseProcess1Geometry, counters.baseProcess1WithRefr);
	func(a_this, a_object, a_arg2);
}

void CullingProbe::Base_Process2::thunk(RE::BSCullingProcess* a_this, const RE::NiCamera* a_camera, RE::NiAVObject* a_scene, RE::NiVisibleArray* a_visibleSet)
{
	ReportFirstHit(hitBaseP2, "BSCullingProcess::Process2");
	(IsMainCamera(a_camera) ? counters.baseProcess2Main : counters.baseProcess2Other).fetch_add(1, std::memory_order_relaxed);
	func(a_this, a_camera, a_scene, a_visibleSet);
	MaybeReport();
}

bool CullingProbe::Base_TestBaseVisibility1::thunk(RE::BSCullingProcess* a_this, RE::BSMultiBound& a_bound)
{
	ReportFirstHit(hitBaseTBV, "BSCullingProcess::TestBaseVisibility1");
	(IsMainCamera(a_this->camera) ? counters.baseTestBaseVisMain : counters.baseTestBaseVisOther).fetch_add(1, std::memory_order_relaxed);
	return func(a_this, a_bound);
}

void CullingProbe::Para_Process1::thunk(RE::BSParabolicCullingProcess* a_this, RE::NiAVObject* a_object, std::int32_t a_arg2)
{
	ReportFirstHit(hitParaP1, "BSParabolicCullingProcess::Process1");
	CountProcess1(a_object, IsMainCamera(a_this->camera),
		counters.paraProcess1Main, counters.paraProcess1Other, counters.paraProcess1Geometry, counters.paraProcess1WithRefr);
	func(a_this, a_object, a_arg2);
}

void CullingProbe::Para_Process2::thunk(RE::BSParabolicCullingProcess* a_this, const RE::NiCamera* a_camera, RE::NiAVObject* a_scene, RE::NiVisibleArray* a_visibleSet)
{
	ReportFirstHit(hitParaP2, "BSParabolicCullingProcess::Process2");
	(IsMainCamera(a_camera) ? counters.paraProcess2Main : counters.paraProcess2Other).fetch_add(1, std::memory_order_relaxed);
	func(a_this, a_camera, a_scene, a_visibleSet);
	MaybeReport();
}

bool CullingProbe::Para_TestBaseVisibility1::thunk(RE::BSParabolicCullingProcess* a_this, RE::BSMultiBound& a_bound)
{
	ReportFirstHit(hitParaTBV, "BSParabolicCullingProcess::TestBaseVisibility1");
	(IsMainCamera(a_this->camera) ? counters.paraTestBaseVisMain : counters.paraTestBaseVisOther).fetch_add(1, std::memory_order_relaxed);
	return func(a_this, a_bound);
}
