#include "FeatureBuffer.h"

#include <array>

#include "Features/CloudShadows.h"
#include "Features/DynamicCubemaps.h"
#include "Features/Effects11.h"
#include "Features/ExponentialHeightFog.h"
#include "Features/ExtendedMaterials.h"
#include "Features/ExtendedTranslucency.h"
#include "Features/GrassLighting.h"
#include "Features/HairBacklighting.h"
#include "Features/HairSpecular.h"
#include "Features/HorizonFix.h"
#include "Features/IBL.h"
#include "Features/LODBlending.h"
#include "Features/LightLimitFix.h"
#include "Features/LinearLighting.h"
#include "Features/NightEye.h"
#include "Features/PostProcessing.h"
#include "Features/LivingWindows.h"
#include "Features/PhysicalSky.h"
#include "Features/ProceduralNightSky.h"
#include "Features/ProceduralSun.h"
#include "Features/ProceduralAurora.h"
#include "Features/RainRefraction.h"
#include "Features/ScreenSpaceGI.h"
#include "Features/ScreenSpaceReflections.h"
#include "Features/Skin.h"
#include "Features/SnowCover.h"
#include "Features/Skylighting.h"
#include "Features/TerrainBlending.h"
#include "Features/TerrainShadows.h"
#include "Features/TerrainVariation.h"
#include "Features/VanillaFresnel.h"
#include "Features/VolumetricLighting.h"
#include "Features/WetnessEffects.h"
#include "TruePBR.h"

template <class... Ts>
std::pair<unsigned char*, size_t> _GetFeatureBufferData(Ts... feat_datas)
{
	// The packed size is a compile-time constant, so reuse one aligned, thread-local buffer
	// instead of allocating/freeing every UpdateSharedData call. The returned pointer is
	// non-owning and must NOT be deleted by the caller.
	constexpr size_t totalSize = (... + sizeof(Ts));
	alignas(16) static thread_local std::array<unsigned char, totalSize> storage;
	size_t offset = 0;

	([&] {
		*reinterpret_cast<decltype(feat_datas)*>(storage.data() + offset) = feat_datas;
		offset += sizeof(decltype(feat_datas));
	}(),
		...);

	return std::make_pair(storage.data(), storage.size());
}

std::pair<unsigned char*, size_t> GetFeatureBufferData(bool a_inWorld)
{
	// The shared buffer is a hand-written mirror of SharedData.hlsli: if any struct's size drifts,
	// every struct after it is read at the wrong offset by every shader, which looks like the whole
	// world breaking and is unaffected by turning features off. Report the sizes once so a mismatch
	// can be seen rather than guessed at.
	static std::once_flag reportOnce;
	std::call_once(reportOnce, [] {
		logger::warn("[FeatureBuffer] tail struct sizes: SnowCover {}, PhysicalSky {}, LivingWindows {}",
			sizeof(SnowCover::PerFrame),
			sizeof(PhysicalSky::CbData),
			sizeof(LivingWindows::GPUSettings));
	});

	return _GetFeatureBufferData(
		globals::features::grassLighting.settings,
		globals::features::extendedMaterials.settings,
		globals::features::dynamicCubemaps.GetCommonBufferData(),
		globals::features::terrainShadows.GetCommonBufferData(),
		globals::features::lightLimitFix.GetCommonBufferData(),
		globals::features::wetnessEffects.GetCommonBufferData(),
		globals::features::skylighting.GetCommonBufferData(a_inWorld),
		globals::features::cloudShadows.GetCommonBufferData(),
		globals::features::proceduralSun.GetCommonBufferData(),
		globals::features::lodBlending.settings,
		globals::features::hairSpecular.settings,
		globals::features::terrainVariation.settings,
		globals::features::ibl.GetCommonBufferData(),
		globals::features::extendedTranslucency.GetCommonBufferData(),
		globals::features::linearLighting.GetCommonBufferData(),
		globals::features::effects11.GetCommonBufferData(),
		globals::features::terrainBlending.GetCommonBufferData(),
		globals::features::exponentialHeightFog.GetCommonBufferData(),
		globals::features::truePBR.settings,
		globals::features::skin.GetCommonBufferData(),
		globals::features::vanillaFresnel.settings,
		globals::features::snowCover.GetCommonBufferData(),
		globals::features::physicalSky.cbData,
		globals::features::livingWindows.GetCommonBufferData(),
		globals::features::volumetricLighting.GetCommonBufferData(),
		globals::features::horizonFix.GetCommonBufferData(),
		globals::features::hairBacklighting.settings,
		globals::features::rainRefraction.GetCommonBufferData(),
		globals::features::nightEye.GetCommonBufferData(),
		globals::features::proceduralAurora.GetCommonBufferData(),
		globals::features::proceduralNightSky.GetCommonBufferData(),
		globals::features::screenSpaceGI.GetCommonBufferData(),
		globals::features::screenSpaceReflections.GetCommonBufferData(),
		globals::features::postProcessing.GetCommonBufferData());
}
