#pragma once
#include <mutex>
#include <unordered_map>

#include <BS_thread_pool.hpp>

/** @brief Manages cached water placement instructions for all worldspaces and LOD levels. */
class WaterCache
{
private:
	struct RuntimeCache;

public:
	/** @brief Describes a single water tile placement with position, size, and water form data. */
	struct Instruction
	{
		union Form
		{
			uint32_t id;
			RE::TESWaterForm* ptr = nullptr;
		};

		uint32_t lodLevel{};
		Form form{};
		int32_t x{};
		int32_t y{};
		uint32_t size{};
		float waterHeight{};
	};

	/** @brief Instruction list for a LOD chunk, plus the runtime cache instance backing it so the caller can keep it alive. */
	struct InstructionResult
	{
		std::shared_ptr<RuntimeCache> cache;
		std::vector<Instruction>* instructions = nullptr;
	};

	/** @brief Thread-safe snapshot of asynchronous cache build progress. */
	struct BuildProgressSnapshot
	{
		uint32_t total{};
		uint32_t completed{};
		uint32_t failed{};
		uint32_t done{};
		bool active{};
		int64_t elapsedMs{};
	};

	/**
	 * @brief Sets the active worldspace and loads its runtime cache.
	 * @param worldSpace The worldspace to activate, or nullptr to clear.
	 * @return True if the worldspace cache was successfully activated.
	 */
	bool SetCurrentWorldSpace(const RE::TESWorldSpace* worldSpace);
	/**
	 * @brief Retrieves water placement instructions for a specific LOD chunk.
	 * @param worldSpace The worldspace to query.
	 * @param lodLevel The LOD level (0 = closest).
	 * @param x The chunk X coordinate.
	 * @param y The chunk Y coordinate.
	 * @return Result with a null instructions pointer if unavailable; otherwise the cache keeps the instructions alive for as long as the result is held.
	 */
	InstructionResult GetInstructions(const RE::TESWorldSpace* worldSpace, uint32_t lodLevel, uint32_t x, uint32_t y);

	/** @brief Generates precache height data for Tamriel using extended data sets. */
	static void GenerateTamrielPrecache();
	/** @brief Loads the caches still valid for this load order and asynchronously rebuilds only the stale worldspaces. */
	bool LoadOrGenerateCaches();
	/** @brief Deletes every cache, including the per-plugin cell stores, then regenerates and reloads them. */
	bool RegenerateCaches();
	/** @brief Generates disk caches for all valid worldspaces asynchronously. */
	bool GenerateCaches();
	/**
	 * @brief Generates disk caches for the given worldspaces asynchronously, then reloads every cache.
	 * @param worldSpaces The worldspaces to rebuild.
	 */
	bool GenerateCaches(const std::vector<RE::TESWorldSpace*>& worldSpaces);

	/** @brief Returns whether an asynchronous cache build is currently running. */
	bool IsBuildRunning() const { return async.running.load(); }
	/** @brief Returns whether the last asynchronous cache build failed. */
	bool HasBuildFailed() const { return async.failed.load(); }
	/** @brief Returns a thread-safe snapshot of the current build progress. */
	BuildProgressSnapshot GetBuildProgressSnapshot() const { return buildProgress.Snapshot(); }

private:
	struct WorldSpaceHeader
	{
		struct MinMax
		{
			int32_t minX{};
			int32_t minY{};
			int32_t maxX{};
			int32_t maxY{};
		};

		uint32_t label = Util::FCC("WTCH");
		int32_t width{};
		int32_t height{};
		MinMax bounds{};
		int32_t dataCount{};
	};

	struct Heights
	{
		float land = FLT_MAX;
		float water = FLT_MAX;
	};

	struct CellData
	{
		Heights heights{};
		RE::FormID formID{};
		RE::TESWaterForm* form = nullptr;
	};

	// Precache contains height data generated with sheson's extended Tamriel data set
	struct PreCache
	{
		WorldSpaceHeader header;
		// Cell XY map
		std::vector<Heights> heights;
	};

	// Per WorldSpace DiskCache that contains a list of instructions for water placement for all LOD levels in series
	struct DiskCache
	{
		WorldSpaceHeader header;
		std::vector<Instruction> instructions;
	};

	// Per WorldSpace RuntimeCache that is the disk cache processed into a runtime optimised format with fast lookups
	struct RuntimeCache
	{
		WorldSpaceHeader header;
		// LODLevel -> LODChunk -> Instructions
		std::vector<std::vector<std::vector<Instruction>>> instructions;

		std::vector<Instruction>* GetInstructions(int32_t lodLevel, int32_t x, int32_t y);
	};

	// What identifies a plugin's content on disk
	struct PluginStamp
	{
		uint64_t size{};
		uint64_t writeTime{};

		bool operator==(const PluginStamp&) const = default;
	};
	using PluginStamps = std::unordered_map<const RE::TESFile*, PluginStamp>;

	// One CELL as a plugin writes it. The form ID is the plugin's own, so this stays true whatever the load order.
	struct PluginCell
	{
		int32_t x{};
		int32_t y{};
		float water = FLT_MAX;   // XCLW; FLT_MAX when the record has none
		float land = FLT_MAX;    // Lowest VHGT height; FLT_MAX when its LAND has none
		RE::FormID rawFormID{};  // XCWT
		uint32_t hasLand{};
	};
	static_assert(sizeof(PluginCell) == 24);

	// Every CELL one plugin defines in one worldspace, within the bounds it was scanned for
	struct PluginCells
	{
		PluginStamp stamp{};
		WorldSpaceHeader::MinMax scanned{};
		std::vector<PluginCell> cells;
	};

	// Per WorldSpace CellStore, by plugin name, so that only the plugins that changed have to be read again.
	// Plugins that left the load order stay in it: switching profiles or re-enabling a mod then costs nothing.
	using CellStore = std::unordered_map<std::string, PluginCells>;

	// The CELL and LAND a cell ends up with across a worldspace's plugins
	struct MergedCell
	{
		int32_t waterFile = -1;
		float water = FLT_MAX;
		float land = FLT_MAX;
		RE::FormID rawFormID{};
		bool hasLand{};
	};

	struct AsyncBuild
	{
		std::unique_ptr<BS::thread_pool<>> pool;
		std::jthread monitor;
		std::atomic<bool> running{ false };
		std::atomic<bool> failed{ false };
		// Caches built this session that could not be written: LoadCaches uses them so the worldspace keeps its water
		std::mutex unwrittenMutex;
		std::unordered_map<std::string, DiskCache> unwritten;
	};

	struct BuildProgress
	{
		std::atomic<uint32_t> total{ 0 };
		std::atomic<uint32_t> completed{ 0 };
		std::atomic<uint32_t> failed{ 0 };
		std::atomic<bool> active{ false };
		std::chrono::steady_clock::time_point t0{};

		void Start(uint32_t tot)
		{
			total.store(tot, std::memory_order_relaxed);
			completed.store(0, std::memory_order_relaxed);
			failed.store(0, std::memory_order_relaxed);
			t0 = std::chrono::steady_clock::now();
			active.store(true, std::memory_order_relaxed);
		}

		void Done(bool ok)
		{
			if (ok)
				completed.fetch_add(1, std::memory_order_relaxed);
			else
				failed.fetch_add(1, std::memory_order_relaxed);
		}

		void Stop() { active.store(false, std::memory_order_relaxed); }

		uint32_t DoneCount() const
		{
			return completed.load(std::memory_order_relaxed) + failed.load(std::memory_order_relaxed);
		}

		int64_t ElapsedMs() const
		{
			return duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
		}

		BuildProgressSnapshot Snapshot() const
		{
			BuildProgressSnapshot s;
			s.total = total.load(std::memory_order_relaxed);
			s.completed = completed.load(std::memory_order_relaxed);
			s.failed = failed.load(std::memory_order_relaxed);
			s.done = s.completed + s.failed;
			s.active = active.load(std::memory_order_relaxed);
			s.elapsedMs = duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
			return s;
		}
	};

	BuildProgress buildProgress;
	AsyncBuild async;

	using CacheMap = std::unordered_map<std::string, std::shared_ptr<RuntimeCache>>;

	std::atomic<std::shared_ptr<const CacheMap>> cacheMap{ std::make_shared<CacheMap>() };

	// Guards currentCache/currentWorldSpace against concurrent terrain-streaming and cache-reload threads.
	std::mutex currentCacheMutex;
	std::shared_ptr<RuntimeCache> currentCache;
	std::string currentWorldSpace;

	// Inputs of every worldspace signature. Written only while no build is running.
	PluginStamps pluginStamps;
	uint64_t waterFormsHash{};

	/** @brief Same as SetCurrentWorldSpace, but assumes currentCacheMutex is already held. */
	bool SetCurrentWorldSpaceLocked(const RE::TESWorldSpace* worldSpace);

	bool LoadCaches();
	/** @brief Refreshes the plugin stamps and water form hash that worldspace signatures are computed from. */
	void UpdateSignatureInputs(const std::vector<RE::TESWorldSpace*>& worldSpaces);
	/** @brief Returns the valid worldspaces whose disk cache is missing or was built from different inputs. */
	std::vector<RE::TESWorldSpace*> FindStaleWorldSpaces();
	/** @brief Hashes everything a worldspace's disk cache depends on. */
	static uint64_t ComputeSignature(RE::TESWorldSpace* worldSpace, const PluginStamps& stamps, uint64_t waterForms);

	static void BuildPreCache(RE::TESWorldSpace* worldSpace, PreCache& cache);
	static bool BuildDiskCache(RE::TESWorldSpace* worldSpace, DiskCache& diskCache, const PluginStamps& stamps, bool verifyCells, bool& outComplete);
	/** @brief Fills the merged cells of a worldspace from its cell store, scanning only the plugins the store does not cover. */
	static void BuildMergedCells(RE::TESWorldSpace* worldSpace, const WorldSpaceHeader& header, const PluginStamps& stamps, std::vector<MergedCell>& outCells, bool& outComplete);
	/**
	 * @brief Bounds of every exterior cell the worldspace's plugins define, for a worldspace whose record has none.
	 * Parses the plugins not yet in the cell store and saves it. Returns false when no plugin defines a cell.
	 */
	static bool TryDeriveBoundsFromCells(RE::TESWorldSpace* worldSpace, const PluginStamps& stamps, WorldSpaceHeader::MinMax& outBounds, bool& outComplete);
	/** @brief Reads every exterior CELL a plugin defines in the worldspace straight from the plugin file. */
	static bool TryReadPluginCells(const RE::TESWorldSpace* worldSpace, const RE::TESFile* file, PluginCells& outCells);
	/** @brief Reads every CELL a plugin defines within the bounds through the engine, for a plugin TryReadPluginCells could not parse. */
	static bool ScanPlugin(RE::TESWorldSpace* worldSpace, RE::TESFile* sourceFile, const WorldSpaceHeader::MinMax& bounds, PluginCells& outCells);
	/** @brief Gives a merged cell the result TryGetCellData gives for that cell. */
	static bool ResolveMergedCell(const RE::TESWorldSpace* worldSpace, RE::TESFileArray* files, const MergedCell& cell, RE::FormID& outFormID, float& outWaterHeight, float& outLandHeight);
	static bool TryReadCellStore(const std::string& editorID, CellStore& store);
	static bool TryWriteCellStore(const std::string& editorID, const CellStore& store);
	static void GenerateInstructions(int32_t lodLevel, DiskCache& diskCache, const std::vector<CellData>& cellData, int32_t& instructionCount);
	static bool TryBuildRuntimeCache(const DiskCache& diskCache, RuntimeCache& cache);
	static std::vector<RE::TESWorldSpace*> GetValidWorldSpaces();
	static void GetLODCoords(int32_t lodLevel, int32_t x, int32_t y, int32_t& outX, int32_t& outY);

	static bool TryGetCellData(RE::TESWorldSpace* worldSpace, RE::TESFileArray* files, int32_t x, int32_t y, RE::FormID& outFormID, float& outWaterHeight, float& outLandHeight, bool resolveFormID);
	static void ReadWaterData(RE::TESFile* file, float& waterHeight, RE::FormID& formID);
	static void ReadMinLandHeightData(RE::TESFile* file, float& minHeight);
	/** @brief Lowers minHeight to the lowest point of a VHGT subrecord. */
	static void GetMinLandHeight(const void* vhgt, float& minHeight);

	template <class T>
	static bool TryWriteCacheToFile(const std::string& name, const WorldSpaceHeader& header, const std::vector<T>& vec);

	template <class T>
	static bool TryReadCacheFromFile(const std::string& name, WorldSpaceHeader& header, std::vector<T>& vec);
};