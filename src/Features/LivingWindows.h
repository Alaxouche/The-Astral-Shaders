#pragma once

#include <array>
#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @brief Living Windows: old-glass optics and parallax interiors behind recognized window panes.
 *
 * Windows are classified once per material from texture-path tokens, the glow map, the mesh name and
 * optional rule files. The per-draw result travels in ExtraFeatureDescriptor bits 22-28, so a
 * non-window draw only clears a few bits and nothing is uploaded or bound per draw. The shared
 * artwork atlases are bound once per frame at pixel shader slots t104-t109.
 */
// The feature holds several 16-byte-aligned GPU mirrors, so the compiler pads around them. That is
// the point of the alignment, not a mistake worth failing the build over.
#pragma warning(push)
#pragma warning(disable: 4324)
struct LivingWindows : Feature
{
	virtual inline std::string GetName() override { return "Living Windows"; }
	virtual std::string GetDisplayName() override { return T("feature.living_windows.name", "Living Windows"); }
	virtual inline std::string GetShortName() override { return "LivingWindows"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kMaterials; }
	virtual inline std::string_view GetShaderDefineName() override { return "LIVING_WINDOWS"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type a_type) override { return a_type == RE::BSShader::Type::Lighting; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			T("feature.living_windows.description", "Turns architectural windows into real openings: old glass with reflections and grime, and furnished rooms with curtains and inhabitants behind the panes."),
			{ T("feature.living_windows.key_feature_1", "Three-dimensional room interiors that follow the viewing angle"),
				T("feature.living_windows.key_feature_2", "Windows light up and go dark through the night, each on its own schedule"),
				T("feature.living_windows.key_feature_3", "Curtains and occupants on separate depth layers"),
				T("feature.living_windows.key_feature_4", "Works with texture replacers and modded buildings, with optional rule files") }
		};
	}

	/** @brief Persisted user settings. */
	struct Settings
	{
		bool Enabled = true;
		bool GlassOptics = true;
		bool Interiors = true;
		bool Occupants = true;
		bool Curtains = true;
		bool OutdoorViews = true;
		bool DebugOverlay = false;
		/** @brief Room depth in half aperture widths. */
		float RoomDepth = 1.4f;
		float InteriorBrightness = 1.0f;
		/** @brief Share of windows that are lit once night falls. */
		float NightLitShare = 0.6f;
		float OccupantActivity = 0.45f;
		float CurtainShare = 0.65f;
		float GlassRoughness = 0.08f;
		float GlassReflectance = 1.0f;
		float GrimeStrength = 0.35f;
		float Waviness = 0.3f;
		/** @brief Rain beading and running down the outside of the pane. */
		bool RainDroplets = true;
		float DropletStrength = 0.7f;
		/** @brief Ice crystals creeping in from the frame in freezing weather. */
		bool Frost = true;
		float FrostStrength = 0.8f;

		// The inside of the same cold pane. Frost grows on the outside from the frame inwards;
		// breath and a hearth fog the middle of the glass from within, so the two are complements
		// rather than alternatives and share the same cold signal.
		bool Condensation = true;
		float CondensationStrength = 0.7f;
		float FadeStart = 2500.0f;
		float FadeEnd = 7000.0f;
		float OutdoorBrightness = 1.0f;
		/** @brief Geometry bound radius below which a window keeps glass optics only. */
		float MinRoomRadius = 18.0f;
		/** @brief Geometry bound radius below which a window shows no occupants. */
		float MinOccupantRadius = 40.0f;
		/** @brief Extra case-insensitive path fragments that mark a material as a window. */
		std::vector<std::string> ExtraWindowTokens;
		/** @brief Case-insensitive path fragments that never count as windows. */
		std::vector<std::string> ExcludedTokens;
	};
	Settings settings;

	/** @brief GPU mirror appended to FeatureData (b6). Layout must match LivingWindowsSettings in SharedData.hlsli. */
	struct alignas(16) GPUSettings
	{
		uint Enabled;
		uint GlassOptics;
		uint Interiors;
		uint Occupants;

		uint Curtains;
		uint OutdoorViews;
		uint DebugOverlay;
		uint AtlasFlags;

		float RoomDepth;
		float InteriorBrightness;
		float NightLitShare;
		float OccupantActivity;

		float CurtainShare;
		float GlassRoughness;
		float GlassReflectance;
		float GrimeStrength;

		float Waviness;
		float FadeStart;
		float FadeEnd;
		float OutdoorBrightness;

		float NightBlend;
		float Raining;
		float GameHour;
		float DropletStrength;

		float FrostStrength;
		float FrostAmount;
		float CondensationStrength;
		float pad0;
	};
	STATIC_ASSERT_ALIGNAS_16(GPUSettings);

	/** @brief ExtraFeatureDescriptor layout shared with Permutation.hlsli. */
	static constexpr std::uint32_t kCandidateBit = 1u << 22;
	static constexpr std::uint32_t kTierShift = 23;
	static constexpr std::uint32_t kFamilyShift = 25;
	static constexpr std::uint32_t kExplicitBit = 1u << 28;
	static constexpr std::uint32_t kDescriptorMask = kCandidateBit | (0b11u << kTierShift) | (0b111u << kFamilyShift) | kExplicitBit;

	/** @brief Most windows one draw publishes; the nearest to the camera win. Matches LivingWindows.hlsli. */
	static constexpr std::uint32_t kMaxWindowsPerDraw = 16;
	/** @brief PS constant buffer slot of the per-draw window list. */
	static constexpr UINT kWindowListSlot = 13;

	/** @brief Per-draw window list, world-space boxes; mirrors LivingWindowList in LivingWindows.hlsli. */
	struct alignas(16) WindowListCB
	{
		std::uint32_t Count;
		std::uint32_t pad0[3];
		float4 Center[kMaxWindowsPerDraw];
		float4 HalfExtent[kMaxWindowsPerDraw];
	};
	STATIC_ASSERT_ALIGNAS_16(WindowListCB);

	/** @brief Atlas availability flags, mirrored in LivingWindows.hlsli. */
	enum AtlasFlag : uint
	{
		kAtlasRoom = 1,
		kAtlasCurtain = 2,
		kAtlasOccupant = 4,
		kAtlasOutdoorDay = 8,
		kAtlasOutdoorNight = 16,
		kAtlasGrime = 32
	};

	/** @brief Returns the per-frame settings uploaded to the shared feature buffer. */
	GPUSettings GetCommonBufferData();

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void SetupResources() override;
	virtual void Prepass() override;
	/** @brief Eases the rain and frost signals once per frame; Prepass calls it, GetCommonBufferData only reads them. */
	void UpdateWeatherSmoothing();
	virtual void PostPostLoad() override;

	/**
	 * @brief Classifies the draw's material and publishes the window bits for the Lighting pixel shader.
	 * @param a_pass The render pass about to be drawn; null clears the bits.
	 */
	void UpdatePermutation(RE::BSRenderPass* a_pass);

private:
	/** @brief Cached material verdict. tier: 0 none, 1 glass only, 2 room, 3 room with occupants. */
	struct Classification
	{
		std::uint8_t tier = 0;
		std::uint8_t family = 0;
		bool explicitWindow = false;
		/** @brief Glow-mapped architecture without window tokens: the mesh name decides per draw. */
		bool checkMeshName = false;
	};

	/** @brief One window of a glass mesh: a model-space box around panes closer together than a mullion. */
	struct WindowBox
	{
		RE::NiPoint3 center;
		RE::NiPoint3 halfExtent;
	};

	/** @brief The windows of one glass mesh, shared by every reference drawing it. */
	struct ShapeWindows
	{
		std::uint32_t vertexCount = 0;
		std::uint32_t triangleCount = 0;
		std::uint64_t vertexDesc = 0;
		std::vector<WindowBox> windows;
	};

	/** @brief A glass mesh's GPU buffers copied for reading once the GPU has caught up. */
	struct PendingReadback
	{
		winrt::com_ptr<ID3D11Buffer> vertices;
		winrt::com_ptr<ID3D11Buffer> indices;
		std::uint32_t vertexCount = 0;
		std::uint32_t triangleCount = 0;
		std::uint64_t desc = 0;
		std::uint32_t stride = 0;
		bool fullPrecision = true;
		std::uint32_t frame = 0;
	};

	/** @brief Groups a mesh's triangles into panes, and panes closer than a mullion into windows (model space). */
	static std::vector<WindowBox> ClusterWindows(const std::uint8_t* a_vertices, std::uint32_t a_stride, bool a_fullPrecision,
		std::uint32_t a_vertexCount, const std::uint16_t* a_indices, std::uint32_t a_triangleCount);

	/**
	 * @brief Finds the windows of a glass mesh once per mesh, from its CPU vertex copy or, lacking
	 * one, from an asynchronous read-back of its GPU buffers.
	 * @return Null while the mesh is not (yet) analysed or is not plain static geometry.
	 */
	const ShapeWindows* GetShapeWindows(RE::BSGeometry* a_geometry);
	/** @brief Copies a mesh's vertex and index buffers to staging buffers for a later read. */
	void QueueWindowReadback(RE::BSGraphics::TriShape* a_data, std::uint32_t a_vertexCount, std::uint32_t a_triangleCount,
		std::uint64_t a_desc, std::uint32_t a_stride, bool a_fullPrecision);
	/** @brief Reads back the copies the GPU has finished, never waiting for the rest. */
	void PollWindowReadbacks();
	/** @brief Uploads the draw's windows in world space to PS b13, skipping unchanged lists. */
	void PublishWindowList(RE::BSGeometry* a_geometry, const ShapeWindows* a_windows);

	Classification ClassifyMaterial(const RE::BSLightingShaderMaterialBase* a_material) const;
	const Classification& GetClassification(const RE::BSLightingShaderMaterialBase* a_material);
	std::uint32_t TierFromMeshName(const char* a_name) const;
	void LoadRuleFiles();
	void LoadAtlases();
	void RefreshUserTokens();

	static constexpr UINT kFirstAtlasSlot = 104;
	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 6> atlasSRVs;
	uint atlasFlags = 0;

	std::unordered_map<std::uint64_t, Classification> classificationCache;

	// Keyed by the mesh's shared renderer data, which every clone of a model points at.
	std::unordered_map<const void*, ShapeWindows> shapeWindowCache;
	std::unordered_map<const void*, PendingReadback> pendingReadbacks;
	std::uint32_t windowMeshesFromCpu = 0;
	std::uint32_t windowMeshesFromGpu = 0;
	std::uint32_t windowMeshesUnreadable = 0;
	std::uint32_t windowsFound = 0;
	winrt::com_ptr<ID3D11Buffer> windowListBuffer;
	WindowListCB publishedWindows{};
	bool windowListPublished = false;
	std::vector<std::pair<float, std::uint32_t>> windowSortScratch;
	std::atomic<bool> classificationDirty = false;
	std::size_t loggedClassifications = 0;

	std::vector<std::string> windowRuleTokens;
	std::vector<std::string> excludedRuleTokens;
	std::size_t ruleFileCount = 0;

	float smoothedRain = 0.0f;
	float smoothedFrost = 0.0f;
	/**
	 * @brief How cold it is where the player stands, 0-1, eased like the others.
	 *
	 * Separate from smoothedFrost because the two answer different questions. Frost grows on the
	 * outside of the pane while snow is falling, so snowfall is the right signal for it. Breath and
	 * a hearth fog the inside whenever the glass is cold, and the coldest, clearest nights are
	 * exactly the ones with no snow falling at all -- driving condensation off snowfall meant it
	 * never appeared on the nights it belongs to.
	 */
	float smoothedCold = 0.0f;
};
#pragma warning(pop)
