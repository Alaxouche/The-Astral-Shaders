#include "WaterCache.h"

#include <BS_thread_pool.hpp>
#include <cmath>
#include <unordered_set>
#include <zlib.h>

#include "Utils/WinApi.h"

namespace
{
	// Far beyond normal terrain heights, but below sentinel/garbage values
	constexpr float kMaxValidCellHeight = 50000.0f;

	bool IsValidCellHeight(const float height)
	{
		// Cell records may use sentinel or garbage heights for absent water data
		return std::isfinite(height) &&
		       height != FLT_MIN &&
		       height != FLT_MAX &&
		       std::fabs(height) < kMaxValidCellHeight;
	}

	RE::TESWaterForm* LookupWaterForm(const RE::FormID formID)
	{
		if (!formID)
			return nullptr;

		auto* form = RE::TESWaterForm::LookupByID<RE::TESWaterForm>(formID);
		if (!form || form->formType != RE::FormType::Water)
			return nullptr;

		return form;
	}

	// Bump whenever the generator's output changes for the same plugins: every signature and cell store is then rebuilt.
	constexpr uint32_t kCacheVersion = 2;

	constexpr uint32_t kCellStoreLabel = Util::FCC("WCDB");
	constexpr uint64_t kHashOffset = 14695981039346656037ull;
	constexpr uint64_t kHashPrime = 1099511628211ull;

	void HashBytes(uint64_t& hash, const void* data, const size_t size)
	{
		const auto* bytes = static_cast<const unsigned char*>(data);
		for (size_t i = 0; i < size; ++i) {
			hash ^= bytes[i];
			hash *= kHashPrime;
		}
	}

	template <class T>
	void HashValue(uint64_t& hash, const T& value)
	{
		static_assert(std::has_unique_object_representations_v<T> || std::is_floating_point_v<T>);
		HashBytes(hash, &value, sizeof(value));
	}

	void HashString(uint64_t& hash, const std::string_view value)
	{
		HashBytes(hash, value.data(), value.size());
		HashValue(hash, value.size());
	}

	// File names are case-insensitive on Windows and the same plugin can reach the game spelled either way from one
	// launch to the next (two mods shipping it, tools rewriting plugins.txt), so identity never depends on the spelling.
	std::string GetPluginKey(const RE::TESFile* file)
	{
		std::string key{ file->fileName };
		std::ranges::transform(key, key.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return key;
	}

	std::filesystem::path GetSignaturePath(const std::string& editorID)
	{
		return Util::PathHelpers::GetUnifiedWaterCachePath() / std::format("{}_cache.hash", editorID);
	}

	bool TryReadSignature(const std::string& editorID, uint64_t& signature)
	{
		std::vector<char> bytes;
		if (Util::FileHelpers::ReadWholeFile(GetSignaturePath(editorID), bytes) != 0 || bytes.size() != sizeof(signature))
			return false;
		std::memcpy(&signature, bytes.data(), sizeof(signature));
		return true;
	}

	bool WriteSignature(const std::string& editorID, const uint64_t signature)
	{
		return Util::FileHelpers::WriteFileAtomic(GetSignaturePath(editorID), { std::span(reinterpret_cast<const char*>(&signature), sizeof(signature)) });
	}

	std::filesystem::path GetCellStorePath(const std::string& editorID)
	{
		return Util::PathHelpers::GetUnifiedWaterCachePath() / std::format("{}_cells.wcd", editorID);
	}

	// Developer check: with this file in the cache folder, a build also runs the original per-cell probe and reports any difference
	bool IsCellVerificationRequested()
	{
		std::error_code ec;
		return std::filesystem::exists(Util::PathHelpers::GetUnifiedWaterCachePath() / "VerifyCells.flag", ec);
	}

	struct CellStoreHeader
	{
		uint32_t label = kCellStoreLabel;
		uint32_t version = kCacheVersion;
		uint32_t sectionCount{};
		uint32_t padding{};
		uint64_t checksum{};
	};

	// Plugin file layout, https://en.uesp.net/wiki/Skyrim_Mod:Mod_File_Format
	struct RecordHeader
	{
		uint32_t signature;
		uint32_t dataSize;
		uint32_t flags;
		uint32_t formID;
		uint32_t versionControl;
		uint16_t formVersion;
		uint16_t unknown;
	};

	struct GroupHeader
	{
		uint32_t signature;
		uint32_t groupSize;  // Includes this header
		uint32_t label;
		int32_t groupType;
		uint32_t versionControl;
		uint32_t unknown;
	};
	static_assert(sizeof(RecordHeader) == 24 && sizeof(GroupHeader) == 24);

	constexpr uint32_t kGroup = Util::FCC("GRUP");
	constexpr uint32_t kRecordCompressed = 0x00040000;
	constexpr uint32_t kMaxRecordSize = 1 << 26;

	enum GroupType : int32_t
	{
		kTopGroup = 0,
		kWorldChildren = 1,
		kExteriorBlock = 4,
		kExteriorSubBlock = 5,
		kCellChildren = 6,
		kCellTemporaryChildren = 9
	};

	bool TryReadAt(const HANDLE file, const uint64_t offset, void* out, const DWORD size)
	{
		OVERLAPPED overlapped{};
		overlapped.Offset = static_cast<DWORD>(offset);
		overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
		DWORD read = 0;
		return ReadFile(file, out, size, &read, &overlapped) && read == size;
	}

	// The subrecords of a record, inflated into scratch if it is compressed
	bool TryGetRecordData(const char* record, std::vector<char>& scratch, std::span<const char>& outData)
	{
		RecordHeader header;
		std::memcpy(&header, record, sizeof(header));
		const char* data = record + sizeof(header);
		if (!(header.flags & kRecordCompressed)) {
			outData = { data, header.dataSize };
			return true;
		}

		uint32_t inflatedSize = 0;
		if (header.dataSize < sizeof(inflatedSize))
			return false;
		std::memcpy(&inflatedSize, data, sizeof(inflatedSize));
		if (inflatedSize > kMaxRecordSize)
			return false;

		scratch.resize(inflatedSize);
		uLongf written = inflatedSize;
		if (uncompress(reinterpret_cast<Bytef*>(scratch.data()), &written, reinterpret_cast<const Bytef*>(data + sizeof(inflatedSize)), header.dataSize - sizeof(inflatedSize)) != Z_OK)
			return false;

		outData = { scratch.data(), written };
		return true;
	}

	/** @brief Calls fn(type, payload) for each subrecord until it returns false. */
	template <class Fn>
	bool ForEachSubrecord(const std::span<const char> data, Fn&& fn)
	{
		size_t offset = 0;
		uint32_t extendedSize = 0;
		while (data.size() - offset >= 6) {
			uint32_t type = 0;
			uint16_t shortSize = 0;
			std::memcpy(&type, data.data() + offset, sizeof(type));
			std::memcpy(&shortSize, data.data() + offset + sizeof(type), sizeof(shortSize));
			offset += 6;

			const size_t size = extendedSize ? extendedSize : shortSize;
			extendedSize = 0;
			if (data.size() - offset < size)
				return false;

			if (type == Util::FCC("XXXX")) {
				if (size != sizeof(extendedSize))
					return false;
				std::memcpy(&extendedSize, data.data() + offset, sizeof(extendedSize));
			} else if (!fn(type, data.subspan(offset, size))) {
				return true;
			}
			offset += size;
		}
		return true;
	}

	template <class T>
	bool TryConsume(const std::vector<char>& bytes, size_t& offset, T& value)
	{
		if (bytes.size() - offset < sizeof(T))
			return false;
		std::memcpy(&value, bytes.data() + offset, sizeof(T));
		offset += sizeof(T);
		return true;
	}
}

bool WaterCache::SetCurrentWorldSpace(const RE::TESWorldSpace* worldSpace)
{
	std::scoped_lock lock(currentCacheMutex);
	return SetCurrentWorldSpaceLocked(worldSpace);
}

bool WaterCache::SetCurrentWorldSpaceLocked(const RE::TESWorldSpace* worldSpace)
{
	if (!worldSpace) {
		currentCache.reset();
		currentWorldSpace.clear();
		return false;
	}

	while (worldSpace->parentWorld && worldSpace->parentUseFlags.all(RE::TESWorldSpace::ParentUseFlag::kUseWaterData))
		worldSpace = worldSpace->parentWorld;

	const auto newWorldSpace = worldSpace->GetFormEditorID();
	if (currentWorldSpace == newWorldSpace) {
		logger::debug("[Unified Water] [Cache] Runtime cache for {} already active", currentWorldSpace);
		return true;
	}

	currentWorldSpace = newWorldSpace;

	const auto snap = std::atomic_load_explicit(&cacheMap, std::memory_order_acquire);
	if (!snap) {
		logger::error("[Unified Water] [Cache] Failed to get cache snapshot");
		return false;
	}

	const auto it = snap->find(newWorldSpace);
	if (it == snap->end()) {
		logger::error("[Unified Water] [Cache] Failed to get runtime cache for {}", newWorldSpace);
		currentCache.reset();
		currentWorldSpace.clear();
		return false;
	}

	currentCache = it->second;
	currentWorldSpace = std::move(newWorldSpace);
	logger::debug("[Unified Water] [Cache] Runtime cache for {} activated", currentWorldSpace);

	return true;
}

WaterCache::InstructionResult WaterCache::GetInstructions(const RE::TESWorldSpace* worldSpace, const uint32_t lodLevel, const uint32_t x, const uint32_t y)
{
	std::scoped_lock lock(currentCacheMutex);

	if (!SetCurrentWorldSpaceLocked(worldSpace)) {
		logger::error("[Unified Water] [Cache] Failed to set current cache to {} while getting instructions", worldSpace->GetFormEditorID());
		return {};
	}

	// Held alongside the pointer so a concurrent LoadCaches can't free it mid-use.
	InstructionResult result;
	result.cache = currentCache;
	result.instructions = currentCache->GetInstructions(lodLevel, x, y);
	return result;
}

std::vector<WaterCache::Instruction>* WaterCache::RuntimeCache::GetInstructions(const int32_t lodLevel, const int32_t x, const int32_t y)
{
	if (lodLevel <= 0)
		return nullptr;

	const auto lodIndex = std::countr_zero(static_cast<uint32_t>(lodLevel)) - 2;
	if (lodIndex < 0 || lodIndex >= instructions.size())
		return nullptr;

	auto& lodInstructions = instructions[lodIndex];

	const auto& bounds = header.bounds;
	int32_t lodCellX, lodCellY, lodMinX, lodMinY, lodMaxX, lodMaxY;
	GetLODCoords(lodLevel, x, y, lodCellX, lodCellY);
	GetLODCoords(lodLevel, bounds.minX, bounds.minY, lodMinX, lodMinY);
	GetLODCoords(lodLevel, bounds.maxX, bounds.maxY, lodMaxX, lodMaxY);
	const int32_t lodWidth = lodMaxX - lodMinX + 1;

	const auto lodCellIndex = (lodCellY - lodMinY) * lodWidth + lodCellX - lodMinX;
	if (lodCellIndex < 0 || lodCellIndex >= lodInstructions.size())
		return nullptr;

	return &lodInstructions[lodCellIndex];
}

void WaterCache::GenerateTamrielPrecache()
{
	// This function is strictly for developer use for building an offline pre-cache using sheson's SSE Terrain Tamriel Full Extend
	// which adds back missing terrain mesh data
	const auto tamriel = RE::TESForm::LookupByEditorID<RE::TESWorldSpace>("Tamriel");
	if (!tamriel) {
		logger::error("[Unified Water] [Cache] Failed to load Tamriel WorldSpace");
		return;
	}

	PreCache cache;
	WorldSpaceHeader& hdr = cache.header;
	hdr.bounds.minX = -96;
	hdr.bounds.minY = -96;
	hdr.bounds.maxX = 95;
	hdr.bounds.maxY = 96;
	hdr.width = cache.header.bounds.maxX - cache.header.bounds.minX + 1;
	hdr.height = cache.header.bounds.maxY - cache.header.bounds.minY + 1;
	hdr.dataCount = hdr.width * hdr.height;

	cache.heights.resize(hdr.dataCount);

	BuildPreCache(tamriel, cache);

	TryWriteCacheToFile("Tamriel_precache.wpc", hdr, cache.heights);
}

bool WaterCache::LoadOrGenerateCaches()
{
	const auto stale = FindStaleWorldSpaces();
	if (!stale.empty()) {
		logger::info("[Unified Water] [Cache] {} WorldSpace caches are stale", stale.size());
		return GenerateCaches(stale);
	}

	if (!LoadCaches()) {
		logger::info("[Unified Water] [Cache] Could not load caches - regenerating...");
		return RegenerateCaches();
	}

	return true;
}

void WaterCache::UpdateSignatureInputs(const std::vector<RE::TESWorldSpace*>& worldSpaces)
{
	PluginStamps stamps;
	const auto dataPath = Util::PathHelpers::GetDataPath();

	for (const auto* worldSpace : worldSpaces) {
		const auto* files = worldSpace ? worldSpace->sourceFiles.array : nullptr;
		if (!files)
			continue;

		for (const auto* file : *files) {
			if (!file || stamps.contains(file))
				continue;

			WIN32_FILE_ATTRIBUTE_DATA attributes{};
			PluginStamp stamp;
			if (GetFileAttributesExW((dataPath / file->fileName).c_str(), GetFileExInfoStandard, &attributes)) {
				stamp.size = (static_cast<uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
				stamp.writeTime = (static_cast<uint64_t>(attributes.ftLastWriteTime.dwHighDateTime) << 32) | attributes.ftLastWriteTime.dwLowDateTime;
			} else {
				// What the game saw when it opened the plugin
				stamp.size = (static_cast<uint64_t>(file->fileData.fileSizeHi) << 32) | file->fileData.fileSizeLo;
				std::memcpy(&stamp.writeTime, &file->fileData.lastWriteTime, sizeof(stamp.writeTime));
			}
			stamps.emplace(file, stamp);
		}
	}

	pluginStamps = std::move(stamps);

	// The merge compares water form flags and the disk cache stores runtime form IDs, and neither comes from a worldspace's own plugins
	uint64_t hash = kHashOffset;
	for (const auto* form : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESWaterForm>()) {
		if (!form)
			continue;
		HashValue(hash, form->formID);
		HashValue(hash, form->flags.underlying());
	}
	waterFormsHash = hash;
}

uint64_t WaterCache::ComputeSignature(RE::TESWorldSpace* worldSpace, const PluginStamps& stamps, const uint64_t waterForms)
{
	uint64_t hash = kHashOffset;
	HashValue(hash, kCacheVersion);
	HashValue(hash, waterForms);
	HashString(hash, worldSpace->GetFormEditorID());
	HashValue(hash, worldSpace->formID);
	HashValue(hash, worldSpace->worldWater ? worldSpace->worldWater->formID : 0);
	HashValue(hash, worldSpace->minimumCoords.x);
	HashValue(hash, worldSpace->minimumCoords.y);
	HashValue(hash, worldSpace->maximumCoords.x);
	HashValue(hash, worldSpace->maximumCoords.y);
	HashValue(hash, worldSpace->defaultLandHeight);
	HashValue(hash, worldSpace->defaultWaterHeight);

	if (const auto* files = worldSpace->sourceFiles.array) {
		for (const auto* file : *files) {
			if (!file)
				continue;
			HashString(hash, GetPluginKey(file));
			HashValue(hash, file->GetCombinedIndex());
			if (const auto stamp = stamps.find(file); stamp != stamps.end()) {
				HashValue(hash, stamp->second.size);
				HashValue(hash, stamp->second.writeTime);
			}
		}
	}

	if (worldSpace->editorID == "Tamriel") {
		std::error_code ec;
		const auto precache = Util::PathHelpers::GetUnifiedWaterCachePath() / "Tamriel_precache.wpc";
		HashValue(hash, std::filesystem::file_size(precache, ec));
		const auto writeTime = std::filesystem::last_write_time(precache, ec).time_since_epoch().count();
		HashValue(hash, writeTime);
	}

	return hash;
}

std::vector<RE::TESWorldSpace*> WaterCache::FindStaleWorldSpaces()
{
	const auto worldSpaces = GetValidWorldSpaces();
	UpdateSignatureInputs(worldSpaces);

	std::vector<RE::TESWorldSpace*> stale;
	for (auto* worldSpace : worldSpaces) {
		const auto editorID = worldSpace ? worldSpace->GetFormEditorID() : nullptr;
		if (!editorID || !*editorID)
			continue;

		const std::string key{ editorID };
		std::error_code ec;
		uint64_t stored = 0;
		if (!TryReadSignature(key, stored) || stored != ComputeSignature(worldSpace, pluginStamps, waterFormsHash) ||
			!std::filesystem::exists(Util::PathHelpers::GetUnifiedWaterCachePath() / std::format("{}_cache.wc", key), ec)) {
			stale.push_back(worldSpace);
		}
	}

	return stale;
}

bool WaterCache::RegenerateCaches()
{
	logger::info("[Unified Water] [Cache] Clearing and regenerating caches...");

	namespace fs = std::filesystem;
	const fs::path dir = Util::PathHelpers::GetUnifiedWaterCachePath();

	std::error_code ec;
	fs::create_directories(dir, ec);

	for (const auto& entry : fs::directory_iterator(dir, ec)) {
		if (ec)
			break;
		if (!entry.is_regular_file())
			continue;

		const auto& path = entry.path();
		if (path.extension() != ".wc" && path.extension() != ".wcd" && path.extension() != ".hash")
			continue;
		std::error_code rec;
		fs::remove(path, rec);
		if (rec) {
			logger::warn("[Unified Water] [Cache] Failed to remove '{}': {}", path.string(), rec.message());
		}
	}

	return GenerateCaches();
}

bool WaterCache::LoadCaches()
{
	const auto t0 = std::chrono::steady_clock::now();

	auto worldSpaces = GetValidWorldSpaces();

	auto newCacheMap = std::make_shared<CacheMap>();
	uint32_t unavailableCount = 0;

	for (auto& worldSpace : worldSpaces) {
		const auto editorID = worldSpace ? worldSpace->GetFormEditorID() : nullptr;
		if (!worldSpace || !editorID || !*editorID) {
			logger::warn("[Unified Water] [Cache] WorldSpace has no EditorID - skipping");
			continue;
		}

		std::string key{ editorID };

		if (newCacheMap->contains(key))
			continue;

		DiskCache diskCache;
		const auto fileName = std::format("{}_cache.wc", key);
		if (!TryReadCacheFromFile(fileName, diskCache.header, diskCache.instructions)) {
			logger::info("[Unified Water] [Cache] Could not locate disk cache for {}", key);
			// An unreadable cache must not keep passing its signature check
			DeleteFileW(GetSignaturePath(key).c_str());
			unavailableCount++;
			continue;
		}

		logger::debug("[Unified Water] [Cache] Loaded cache for {} - Bounds {},{}  {},{} - Instructions {}", editorID, diskCache.header.bounds.minX, diskCache.header.bounds.minY, diskCache.header.bounds.maxX, diskCache.header.bounds.maxY, diskCache.header.dataCount);

		auto newCache = std::make_unique<RuntimeCache>();
		if (!TryBuildRuntimeCache(diskCache, *newCache)) {
			logger::warn("[Unified Water] [Cache] Failed to build runtime cache for {}", key);
			unavailableCount++;
			continue;
		}

		newCacheMap->emplace(std::move(key), std::move(newCache));
	}

	if (unavailableCount) {
		logger::info("[Unified Water] [Cache] Loaded {} / {} worldspace caches ({} unavailable)", newCacheMap->size(), worldSpaces.size(), unavailableCount);
	}

	if (newCacheMap->empty()) {
		return false;
	}

	std::atomic_store_explicit(&cacheMap, std::const_pointer_cast<const CacheMap>(newCacheMap), std::memory_order_release);

	{
		std::scoped_lock lock(currentCacheMutex);
		if (!currentWorldSpace.empty()) {
			if (const auto snap = std::atomic_load_explicit(&cacheMap, std::memory_order_acquire)) {
				if (const auto it = snap->find(currentWorldSpace); it != snap->end()) {
					currentCache = it->second;
				} else {
					// Dropped from the reloaded map - clear so a name match can't fast-return stale.
					currentCache.reset();
					currentWorldSpace.clear();
				}
			}
		}
	}

	const auto t1 = std::chrono::steady_clock::now();
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
	logger::info("[Unified Water] [Cache] Caches loaded in {} ms", ms);

	return true;
}

bool WaterCache::GenerateCaches()
{
	return GenerateCaches(GetValidWorldSpaces());
}

bool WaterCache::GenerateCaches(const std::vector<RE::TESWorldSpace*>& worldSpaces)
{
	if (async.running.load()) {
		logger::warn("[Unified Water] [Cache] Build already running");
		return false;
	}

	UpdateSignatureInputs(GetValidWorldSpaces());
	const bool verifyCells = IsCellVerificationRequested();

	{
		namespace fs = std::filesystem;
		std::error_code ec;
		fs::create_directories(Util::PathHelpers::GetUnifiedWaterCachePath(), ec);
		if (ec) {
			logger::error("[Unified Water] [Cache] Failed to ensure output directory: {}", ec.message());
			return false;
		}
	}

	// Use P-core logical count on Intel hybrid CPUs; falls back to hardware_concurrency() on non-hybrid.
	const unsigned hw = Util::GetPerformanceCoreCount();
	const unsigned threads = std::max(1u, hw > 4 ? hw - 4 : (hw * 3) / 4);
	async.pool = std::make_unique<BS::thread_pool<>>(threads);

	async.failed.store(false);
	buildProgress.Start(static_cast<uint32_t>(worldSpaces.size()));
	async.running.store(true);

	logger::info("[Unified Water] [Cache] Starting async disk cache build for {} WorldSpaces on {} threads", buildProgress.total.load(), threads);

	for (auto& worldSpace : worldSpaces) {
		const char* editorID = worldSpace && worldSpace->editorID.c_str() ? worldSpace->editorID.c_str() : "";
		if (!worldSpace || !editorID || !*editorID) {
			logger::warn("[Unified Water] [Cache] WorldSpace has no EditorID - skipping");
			buildProgress.Done(true);
			continue;
		}

		async.pool->detach_task([this, worldSpace, editorID, verifyCells] {
			DiskCache cache = {};
			bool complete = true;
			const auto name = std::format("{}_cache.wc", editorID);
			const auto success = BuildDiskCache(worldSpace, cache, pluginStamps, verifyCells, complete) && TryWriteCacheToFile(name, cache.header, cache.instructions);
			if (success) {
				logger::debug("[Unified Water] [Cache] {} generation complete", editorID);
				// A plugin that could not be read leaves the cache unsigned, so the next launch tries it again
				if (complete && !WriteSignature(editorID, ComputeSignature(worldSpace, pluginStamps, waterFormsHash)))
					logger::warn("[Unified Water] [Cache] Failed to write the signature of {}; it will be rebuilt next launch", editorID);
			} else {
				async.failed.store(true, std::memory_order_relaxed);
				logger::error("[Unified Water] [Cache] {} generation failed", editorID);
			}

			buildProgress.Done(success);
		});
	}

	async.monitor = std::jthread([this](const std::stop_token& st) {
		using namespace std::chrono_literals;
		while (!st.stop_requested()) {
			const auto bp = this->GetBuildProgressSnapshot();
			if (bp.done >= bp.total)
				break;
			std::this_thread::sleep_for(50ms);
		}

		if (async.pool)
			async.pool->wait();

		buildProgress.Stop();

		logger::info("[Unified Water] [Cache] Disk caches generated in {} ms  ({} / {} complete - {} failed)", buildProgress.ElapsedMs(), buildProgress.completed.load(), buildProgress.total.load(), buildProgress.failed.load());
		LoadCaches();
		async.running.store(false);
	});

	return true;
}

void WaterCache::BuildPreCache(RE::TESWorldSpace* worldSpace, PreCache& cache)
{
	const auto& hdr = cache.header;
	const auto& [minX, minY, maxX, maxY] = hdr.bounds;
	auto& heights = cache.heights;

	const auto files = worldSpace->sourceFiles.array;

	for (auto y = minY; y <= maxY; ++y) {
		for (auto x = minX; x <= maxX; ++x) {
			const int32_t idx = (y - minY) * hdr.width + x - minX;
			RE::FormID rawFormID;
			float landHeight, waterHeight;

			TryGetCellData(worldSpace, files, x, y, rawFormID, waterHeight, landHeight, false);
			heights[idx] = { landHeight, waterHeight };
		}
	}
}

bool WaterCache::TryReadPluginCells(const RE::TESWorldSpace* worldSpace, const RE::TESFile* file, PluginCells& outCells)
{
	const auto path = Util::PathHelpers::GetDataPath() / file->fileName;
	winrt::file_handle handle{ CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr) };
	LARGE_INTEGER size{};
	if (!handle || !GetFileSizeEx(handle.get(), &size))
		return false;
	const auto fileSize = static_cast<uint64_t>(size.QuadPart);

	RecordHeader pluginHeader;
	if (fileSize < sizeof(pluginHeader) || !TryReadAt(handle.get(), 0, &pluginHeader, sizeof(pluginHeader)) || pluginHeader.signature != Util::FCC("TES4"))
		return false;

	// Group sizes let every other record in the plugin be skipped without reading it
	std::vector<char> worldChildren;
	uint64_t offset = sizeof(RecordHeader) + static_cast<uint64_t>(pluginHeader.dataSize);
	while (offset + sizeof(GroupHeader) <= fileSize) {
		GroupHeader topGroup;
		if (!TryReadAt(handle.get(), offset, &topGroup, sizeof(topGroup)) || topGroup.signature != kGroup || topGroup.groupSize < sizeof(GroupHeader) || offset + topGroup.groupSize > fileSize)
			return false;

		if (topGroup.groupType == kTopGroup && topGroup.label == Util::FCC("WRLD")) {
			const uint64_t end = offset + topGroup.groupSize;
			uint64_t child = offset + sizeof(GroupHeader);
			while (child + sizeof(GroupHeader) <= end) {
				GroupHeader group;
				if (!TryReadAt(handle.get(), child, &group, sizeof(group)))
					return false;

				if (group.signature != kGroup) {
					child += sizeof(RecordHeader) + static_cast<uint64_t>(group.groupSize);
					continue;
				}

				if (group.groupSize < sizeof(GroupHeader) || child + group.groupSize > end)
					return false;

				if (group.groupType == kWorldChildren && file->GetRuntimeFormID(group.label) == worldSpace->formID) {
					worldChildren.resize(group.groupSize - sizeof(GroupHeader));
					if (!worldChildren.empty() && !TryReadAt(handle.get(), child + sizeof(GroupHeader), worldChildren.data(), static_cast<DWORD>(worldChildren.size())))
						return false;
					break;
				}
				child += group.groupSize;
			}
			break;
		}
		offset += topGroup.groupSize;
	}

	outCells.cells.clear();
	outCells.scanned = { INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX };

	std::vector<char> scratch;
	std::vector<PluginCell>& cells = outCells.cells;
	uint32_t currentCellFormID = 0;
	bool hasCurrentCell = false;

	const auto parse = [&](auto&& self, const char* begin, const char* end, const int32_t parentType) -> bool {
		while (static_cast<size_t>(end - begin) >= sizeof(RecordHeader)) {
			RecordHeader record;
			std::memcpy(&record, begin, sizeof(record));

			if (record.signature == kGroup) {
				GroupHeader group;
				std::memcpy(&group, begin, sizeof(group));
				if (group.groupSize < sizeof(GroupHeader) || group.groupSize > static_cast<size_t>(end - begin))
					return false;

				const bool ownChildren = group.groupType == kCellChildren && hasCurrentCell && group.label == currentCellFormID;
				if ((group.groupType == kExteriorBlock || group.groupType == kExteriorSubBlock || group.groupType == kCellTemporaryChildren || ownChildren) &&
					!self(self, begin + sizeof(GroupHeader), begin + group.groupSize, group.groupType)) {
					return false;
				}
				begin += group.groupSize;
				continue;
			}

			if (record.dataSize > static_cast<size_t>(end - begin) - sizeof(RecordHeader))
				return false;

			if (parentType == kExteriorSubBlock && record.signature == Util::FCC("CELL")) {
				hasCurrentCell = false;
				std::span<const char> data;
				if (!TryGetRecordData(begin, scratch, data))
					return false;

				PluginCell cell;
				bool hasGrid = false;
				bool hasWater = false;
				if (!ForEachSubrecord(data, [&](const uint32_t type, const std::span<const char> payload) {
						if (type == Util::FCC("XCLC") && payload.size() >= 2 * sizeof(int32_t)) {
							std::memcpy(&cell.x, payload.data(), sizeof(cell.x));
							std::memcpy(&cell.y, payload.data() + sizeof(cell.x), sizeof(cell.y));
							hasGrid = true;
						} else if (type == Util::FCC("XCLW") && !hasWater && payload.size() >= sizeof(cell.water)) {
							std::memcpy(&cell.water, payload.data(), sizeof(cell.water));
							hasWater = true;
						} else if (type == Util::FCC("XCWT") && hasWater && !cell.rawFormID && payload.size() >= sizeof(cell.rawFormID)) {
							std::memcpy(&cell.rawFormID, payload.data(), sizeof(cell.rawFormID));
						}
						return true;
					})) {
					return false;
				}

				if (hasGrid) {
					cells.push_back(cell);
					currentCellFormID = record.formID;
					hasCurrentCell = true;
				}
			} else if (parentType == kCellTemporaryChildren && record.signature == Util::FCC("LAND") && hasCurrentCell && !cells.back().hasLand) {
				std::span<const char> data;
				if (!TryGetRecordData(begin, scratch, data))
					return false;

				auto& cell = cells.back();
				cell.hasLand = 1;
				if (!ForEachSubrecord(data, [&](const uint32_t type, const std::span<const char> payload) {
						if (type != Util::FCC("VHGT"))
							return true;
						if (payload.size() >= 1093)
							GetMinLandHeight(payload.data(), cell.land);
						return false;
					})) {
					return false;
				}
			}

			begin += sizeof(RecordHeader) + record.dataSize;
		}
		return true;
	};

	if (!worldChildren.empty() && !parse(parse, worldChildren.data(), worldChildren.data() + worldChildren.size(), kWorldChildren)) {
		outCells.cells.clear();
		return false;
	}
	return true;
}

bool WaterCache::ScanPlugin(RE::TESWorldSpace* worldSpace, RE::TESFile* sourceFile, const WorldSpaceHeader::MinMax& bounds, PluginCells& outCells)
{
	RE::TESFile* file = sourceFile->Duplicate();
	if (!file)
		return false;

	outCells.cells.clear();
	outCells.scanned = bounds;

	for (auto y = bounds.minY; y <= bounds.maxY; ++y) {
		for (auto x = bounds.minX; x <= bounds.maxX; ++x) {
			if (!file->SeekCell(worldSpace, x, y))
				continue;

			PluginCell cell;
			cell.x = x;
			cell.y = y;
			ReadWaterData(file, cell.water, cell.rawFormID);

			if (file->SeekCell(worldSpace, x, y) && file->SeekLandscapeForCurrentCell()) {
				ReadMinLandHeightData(file, cell.land);
				cell.hasLand = 1;
			}

			outCells.cells.push_back(cell);
		}
	}

	return true;
}

void WaterCache::BuildMergedCells(RE::TESWorldSpace* worldSpace, const WorldSpaceHeader& header, const PluginStamps& stamps, std::vector<MergedCell>& outCells, bool& outComplete)
{
	const auto contains = [](const WorldSpaceHeader::MinMax& outer, const WorldSpaceHeader::MinMax& inner) {
		return outer.minX <= inner.minX && outer.minY <= inner.minY && outer.maxX >= inner.maxX && outer.maxY >= inner.maxY;
	};

	const std::string editorID{ worldSpace->GetFormEditorID() };
	CellStore store;
	TryReadCellStore(editorID, store);

	auto* files = worldSpace->sourceFiles.array;
	const auto fileCount = files ? static_cast<int32_t>(files->size()) : 0;
	std::vector<const PluginCells*> plugins(fileCount, nullptr);
	uint32_t scanned = 0;

	for (int32_t i = 0; i < fileCount; ++i) {
		auto* file = files->data()[i];
		const auto stamp = file ? stamps.find(file) : stamps.end();
		if (stamp == stamps.end()) {
			outComplete = false;
			continue;
		}

		const auto key = GetPluginKey(file);
		auto& plugin = store[key];
		if (plugin.stamp != stamp->second || !contains(plugin.scanned, header.bounds)) {
			// The engine's SeekCell randomly misses a few cells of a large master; a cached miss would be a permanent hole
			if (!TryReadPluginCells(worldSpace, file, plugin) && !ScanPlugin(worldSpace, file, header.bounds, plugin)) {
				logger::warn("[Unified Water] [Cache] {}: Could not read {}", editorID, file->fileName);
				store.erase(key);
				outComplete = false;
				continue;
			}
			plugin.stamp = stamp->second;
			scanned++;
		}
		plugins[i] = &plugin;
	}

	logger::debug("[Unified Water] [Cache] {}: {} of {} plugins read, the rest from the cell store", editorID, scanned, fileCount);

	if (scanned && !TryWriteCellStore(editorID, store))
		logger::warn("[Unified Water] [Cache] {}: Failed to write the cell store", editorID);

	outCells.assign(header.width * header.height, {});

	// In load order, so the last plugin defining a cell wins its water and the last one with a LAND wins its height:
	// a LAND always sits under a CELL, which puts it at or before the plugin TryGetCellData stops at for the water.
	for (int32_t i = 0; i < fileCount; ++i) {
		if (!plugins[i])
			continue;

		for (const auto& cell : plugins[i]->cells) {
			if (cell.x < header.bounds.minX || cell.x > header.bounds.maxX || cell.y < header.bounds.minY || cell.y > header.bounds.maxY)
				continue;

			auto& merged = outCells[(cell.y - header.bounds.minY) * header.width + cell.x - header.bounds.minX];
			merged.waterFile = i;
			merged.water = cell.water;
			merged.rawFormID = cell.rawFormID;
			if (cell.hasLand) {
				merged.land = cell.land;
				merged.hasLand = true;
			}
		}
	}
}

bool WaterCache::ResolveMergedCell(const RE::TESWorldSpace* worldSpace, RE::TESFileArray* files, const MergedCell& cell, RE::FormID& outFormID, float& outWaterHeight, float& outLandHeight)
{
	if (cell.waterFile < 0 || !cell.hasLand) {
		outFormID = 0;
		outWaterHeight = FLT_MIN;
		outLandHeight = 0;
		return false;
	}

	outFormID = cell.rawFormID ? files->data()[cell.waterFile]->GetRuntimeFormID(cell.rawFormID) : 0;
	outWaterHeight = cell.water == FLT_MAX ? worldSpace->defaultWaterHeight : cell.water;
	outLandHeight = cell.land == FLT_MAX ? worldSpace->defaultLandHeight : cell.land;
	return true;
}

bool WaterCache::TryReadCellStore(const std::string& editorID, CellStore& store)
{
	std::vector<char> bytes;
	if (Util::FileHelpers::ReadWholeFile(GetCellStorePath(editorID), bytes) != 0)
		return false;

	size_t offset = 0;
	CellStoreHeader header;
	if (!TryConsume(bytes, offset, header) || header.label != kCellStoreLabel || header.version != kCacheVersion)
		return false;

	uint64_t checksum = kHashOffset;
	HashBytes(checksum, bytes.data() + offset, bytes.size() - offset);
	if (checksum != header.checksum) {
		logger::warn("[Unified Water] [Cache] {}: The cell store is corrupt and will be rebuilt", editorID);
		return false;
	}

	for (uint32_t i = 0; i < header.sectionCount; ++i) {
		PluginCells plugin;
		uint32_t nameLength = 0;
		uint32_t cellCount = 0;
		if (!TryConsume(bytes, offset, plugin.stamp) || !TryConsume(bytes, offset, plugin.scanned) ||
			!TryConsume(bytes, offset, nameLength) || !TryConsume(bytes, offset, cellCount) ||
			bytes.size() - offset < nameLength) {
			store.clear();
			return false;
		}

		std::string name(bytes.data() + offset, nameLength);
		offset += nameLength;

		if ((bytes.size() - offset) / sizeof(PluginCell) < cellCount) {
			store.clear();
			return false;
		}

		plugin.cells.resize(cellCount);
		std::memcpy(plugin.cells.data(), bytes.data() + offset, cellCount * sizeof(PluginCell));
		offset += cellCount * sizeof(PluginCell);

		store.insert_or_assign(std::move(name), std::move(plugin));
	}

	return true;
}

bool WaterCache::TryWriteCellStore(const std::string& editorID, const CellStore& store)
{
	std::vector<char> payload;
	const auto append = [&payload](const void* data, const size_t size) {
		payload.insert(payload.end(), static_cast<const char*>(data), static_cast<const char*>(data) + size);
	};

	for (const auto& [name, plugin] : store) {
		const auto nameLength = static_cast<uint32_t>(name.size());
		const auto cellCount = static_cast<uint32_t>(plugin.cells.size());
		append(&plugin.stamp, sizeof(plugin.stamp));
		append(&plugin.scanned, sizeof(plugin.scanned));
		append(&nameLength, sizeof(nameLength));
		append(&cellCount, sizeof(cellCount));
		append(name.data(), name.size());
		append(plugin.cells.data(), plugin.cells.size() * sizeof(PluginCell));
	}

	CellStoreHeader header;
	header.sectionCount = static_cast<uint32_t>(store.size());
	header.checksum = kHashOffset;
	HashBytes(header.checksum, payload.data(), payload.size());

	return Util::FileHelpers::WriteFileAtomic(GetCellStorePath(editorID), { std::span(reinterpret_cast<const char*>(&header), sizeof(header)), std::span<const char>(payload) });
}

bool WaterCache::BuildDiskCache(RE::TESWorldSpace* worldSpace, DiskCache& diskCache, const PluginStamps& stamps, const bool verifyCells, bool& outComplete)
{
	const auto t0 = std::chrono::steady_clock::now();

	diskCache.header = {};
	diskCache.instructions = {};
	auto& hdr = diskCache.header;
	auto& [minX, minY, maxX, maxY] = diskCache.header.bounds;

	const auto editorID = worldSpace->editorID;

	bool hasPrecache = false;
	PreCache preCache;

	const auto wsMin = worldSpace->minimumCoords;
	const auto wsMax = worldSpace->maximumCoords;

	if (editorID == "Tamriel") {
		if (TryReadCacheFromFile("Tamriel_precache.wpc", preCache.header, preCache.heights)) {
			diskCache.header = preCache.header;
			hasPrecache = true;
		} else {
			logger::warn("[Unified Water] [Cache] Tamriel: Failed to load precache, falling back to generation");
			Util::WorldToCell(wsMin, minX, minY);
			Util::WorldToCell(wsMax, maxX, maxY);
			maxX -= 1;
			maxY -= 1;
			hdr.width = maxX - minX + 1;
			hdr.height = maxY - minY + 1;
			hdr.dataCount = 0;
		}
	} else {
		Util::WorldToCell(wsMin, minX, minY);
		Util::WorldToCell(wsMax, maxX, maxY);
		maxX -= 1;
		maxY -= 1;
		hdr.width = maxX - minX + 1;
		hdr.height = maxY - minY + 1;
		hdr.dataCount = 0;
	}

	bool invalidWS = wsMin.x == FLT_MIN || wsMin.y == FLT_MIN || wsMax.x == FLT_MAX || wsMax.y == FLT_MAX || wsMin.x == FLT_MAX || wsMin.y == FLT_MAX || wsMax.x == FLT_MIN || wsMax.y == FLT_MIN;
	invalidWS = invalidWS || hdr.width <= 0 || hdr.height <= 0 || hdr.width > 512 || hdr.height > 512;

	if (invalidWS) {
		logger::warn("[Unified Water] [Cache] {}: Invalid WorldSpace - skipping", editorID.c_str());
		diskCache = {};
		return true;
	}

	logger::debug("[Unified Water] [Cache] {}: Building disk cache...", editorID.c_str());

	const auto files = worldSpace->sourceFiles.array;

	std::vector<MergedCell> mergedCells;
	BuildMergedCells(worldSpace, hdr, stamps, mergedCells, outComplete);

	int32_t verifyMismatchCount = 0;

	auto cellData = std::vector<CellData>(hdr.width * hdr.height);

	int32_t waterCellCount = 0;
	int32_t precacheFallbackCount = 0;
	int32_t skippedMissingCellDataCount = 0;
	int32_t skippedInvalidHeightCount = 0;
	int32_t skippedUnresolvedFormCount = 0;
	int32_t firstUnresolvedFormX = 0;
	int32_t firstUnresolvedFormY = 0;
	RE::FormID firstUnresolvedFormID = 0;

	for (auto y = minY; y <= maxY; ++y) {
		for (auto x = minX; x <= maxX; ++x) {
			const int32_t idx = (y - minY) * hdr.width + x - minX;

			RE::FormID formID = 0;
			float landHeight = FLT_MAX;
			float waterHeight = FLT_MAX;

			const bool hasCellData = ResolveMergedCell(worldSpace, files, mergedCells[idx], formID, waterHeight, landHeight);

			if (verifyCells) {
				RE::FormID probedFormID = 0;
				float probedWater = FLT_MAX;
				float probedLand = FLT_MAX;
				const bool probed = TryGetCellData(worldSpace, files, x, y, probedFormID, probedWater, probedLand, true);
				if (probed != hasCellData || probedFormID != formID || probedWater != waterHeight || probedLand != landHeight) {
					if (verifyMismatchCount < 8) {
						logger::warn("[Unified Water] [Cache] Verify {}: cell {},{} is {} form {:08X} water {} land {}, the probe gives {} form {:08X} water {} land {}",
							editorID.c_str(), x, y, hasCellData, formID, waterHeight, landHeight, probed, probedFormID, probedWater, probedLand);
					}
					verifyMismatchCount++;
				}
			}

			if (!hasCellData) {
				if (!hasPrecache) {
					skippedMissingCellDataCount++;
					cellData[idx] = {};
					continue;
				}

				const auto precacheIdx = static_cast<size_t>(idx);
				if (precacheIdx >= preCache.heights.size()) {
					// Corrupt precache files can have headers that outlive their payload
					skippedMissingCellDataCount++;
					cellData[idx] = {};
					continue;
				}

				const auto [land, water] = preCache.heights[precacheIdx];
				landHeight = land;
				waterHeight = water;
				precacheFallbackCount++;
			}

			const bool validHeights = IsValidCellHeight(waterHeight) && IsValidCellHeight(landHeight);

			if (validHeights && waterHeight > landHeight) {
				if (!formID && worldSpace->worldWater)
					formID = worldSpace->worldWater->formID;  // Use default world water if no water form set
			} else {
				if (!validHeights)
					skippedInvalidHeightCount++;
				// Discard invalid tiles before they can generate floating water planes
				formID = 0;
			}

			const RE::FormID requestedFormID = formID;
			RE::TESWaterForm* form = LookupWaterForm(formID);

			if (!form && requestedFormID && worldSpace->worldWater && requestedFormID != worldSpace->worldWater->formID) {
				formID = worldSpace->worldWater->formID;
				form = LookupWaterForm(formID);
			}

			if (requestedFormID && !form) {
				if (!skippedUnresolvedFormCount) {
					firstUnresolvedFormX = x;
					firstUnresolvedFormY = y;
					firstUnresolvedFormID = requestedFormID;
				}
				skippedUnresolvedFormCount++;
				cellData[idx] = {};
				continue;
			}

			if (form)
				waterCellCount++;

			cellData[idx] = { landHeight, waterHeight, formID, form };
		}
	}

	if (verifyCells)
		logger::warn("[Unified Water] [Cache] Verify {}: {} cells, {} differ from the probe", editorID.c_str(), hdr.width * hdr.height, verifyMismatchCount);

	if (skippedUnresolvedFormCount) {
		logger::warn("[Unified Water] [Cache] {}: Skipped {} cells due to unresolvable water forms (first at {},{} form {:08X})",
			editorID.c_str(), skippedUnresolvedFormCount, firstUnresolvedFormX, firstUnresolvedFormY, firstUnresolvedFormID);
	}

	if (precacheFallbackCount || skippedMissingCellDataCount || skippedInvalidHeightCount) {
		logger::debug("[Unified Water] [Cache] {}: {} cells used precache fallback, {} cells skipped due to missing data, {} cells skipped due to invalid heights",
			editorID.c_str(), precacheFallbackCount, skippedMissingCellDataCount, skippedInvalidHeightCount);
	}

	logger::debug("[Unified Water] [Cache] {}: Generating instructions for {} water cells...", editorID.c_str(), waterCellCount);

	int32_t instructionCount;
	GenerateInstructions(4, diskCache, cellData, instructionCount);
	logger::debug("[Unified Water] [Cache] {}: LOD4 - {} instructions generated", editorID.c_str(), instructionCount);

	GenerateInstructions(8, diskCache, cellData, instructionCount);
	logger::debug("[Unified Water] [Cache] {}: LOD8 - {} instructions generated", editorID.c_str(), instructionCount);

	GenerateInstructions(16, diskCache, cellData, instructionCount);
	logger::debug("[Unified Water] [Cache] {}: LOD16 - {} instructions generated", editorID.c_str(), instructionCount);

	GenerateInstructions(32, diskCache, cellData, instructionCount);
	logger::debug("[Unified Water] [Cache] {}: LOD32 - {} instructions generated", editorID.c_str(), instructionCount);

	diskCache.header.dataCount = static_cast<int32_t>(diskCache.instructions.size());

	const auto t1 = std::chrono::steady_clock::now();
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
	logger::info("[Unified Water] [Cache] {}: Cache generated in {} ms", worldSpace->GetFormEditorID(), ms);
	return true;
}

void WaterCache::GenerateInstructions(const int32_t lodLevel, DiskCache& diskCache, const std::vector<CellData>& cellData, int32_t& instructionCount)
{
	instructionCount = 0;
	const auto& hdr = diskCache.header;
	const auto& [minX, minY, maxX, maxY] = hdr.bounds;
	int32_t lodMinX, lodMinY, lodMaxX, lodMaxY;
	GetLODCoords(lodLevel, minX, minY, lodMinX, lodMinY);
	GetLODCoords(lodLevel, maxX, maxY, lodMaxX, lodMaxY);

	auto& instructions = diskCache.instructions;
	instructions.reserve(lodLevel == 4 ? cellData.size() : cellData.size() / 5);
	std::vector<bool> processed(lodLevel * lodLevel);

	// Loop over all lod chunks
	for (int32_t lodY = lodMinY; lodY <= lodMaxY; ++lodY) {
		for (int32_t lodX = lodMinX; lodX <= lodMaxX; ++lodX) {
			// Chunk base coordinates
			const int32_t baseX = lodX * lodLevel - minX;
			const int32_t baseY = lodY * lodLevel - minY;

			int32_t cellsRemaining = lodLevel * lodLevel;
			std::ranges::fill(processed, false);

			// Loop over all cells within the chunk
			for (int32_t localY = 0; localY < lodLevel; ++localY) {
				for (int32_t localX = 0; localX < lodLevel; ++localX) {
					const size_t localIndex = localY * lodLevel + localX;
					// Skip any processed cells
					if (processed[localIndex])
						continue;

					// Array relative cell coordinates
					const int32_t cellX = baseX + localX;
					const int32_t cellY = baseY + localY;

					// Skip any cells outside the map
					if (cellX < 0 || cellX >= hdr.width || cellY < 0 || cellY >= hdr.height) {
						processed[localIndex] = true;
						cellsRemaining--;
						continue;
					}

					const int32_t cellIndex = cellY * hdr.width + cellX;
					const auto& [heights, formID, form] = cellData[cellIndex];

					// Skip any cells without water
					if (!form) {
						processed[localIndex] = true;
						cellsRemaining--;
						continue;
					}

					// Neighbouring cells must match target values to merge
					const float targetHeight = heights.water;
					const auto targetType = formID;
					const uint8_t targetFlags = form->flags.underlying();

					int32_t size = 1;
					int32_t maxSize = 1;
					if (lodLevel > 4) {
						// For LOD 4 we don't want combined meshes, they need to be toggled off as real cells load
						// For others, size can only grow to what's left in the chunk or the maximum map dimensions
						maxSize = std::min(lodLevel - localX, lodLevel - localY);
						maxSize = std::min(maxSize, hdr.width - cellX);
						maxSize = std::min(maxSize, hdr.height - cellY);
					}

					// Try to grow to the largest square possible starting at localX/localY
					for (int32_t candidateSize = 2; candidateSize <= maxSize; ++candidateSize) {
						bool failed = false;

						// Check new top row
						const int32_t newRowY = localY + candidateSize - 1;
						for (int32_t dx = 0; dx < candidateSize; ++dx) {
							const int32_t rx = baseX + (localX + dx);
							const int32_t ry = baseY + newRowY;
							const int32_t idx = ry * hdr.width + rx;
							const int32_t cLocalIdx = newRowY * lodLevel + localX + dx;
							const auto& [cHeights, cFormID, cForm] = cellData[idx];
							if (processed[cLocalIdx] || !cForm || cHeights.water != targetHeight || cFormID != targetType || cForm->flags.underlying() != targetFlags) {
								failed = true;
								break;
							}
						}

						if (failed)
							break;

						// Check new right column, excluding the corner already checked above
						const int32_t newColX = localX + candidateSize - 1;
						for (int32_t dy = 0; dy < candidateSize - 1; ++dy) {
							const int32_t rx = baseX + newColX;
							const int32_t ry = baseY + (localY + dy);
							const int32_t idx = ry * hdr.width + rx;
							const int32_t cLocalIdx = (localY + dy) * lodLevel + newColX;
							const auto& [cHeights, cFormID, cForm] = cellData[idx];
							if (processed[cLocalIdx] || !cForm || cHeights.water != targetHeight || cFormID != targetType || cForm->flags.underlying() != targetFlags) {
								failed = true;
								break;
							}
						}

						if (failed)
							break;

						// Success for this candidate size
						size = candidateSize;
					}

					// Mark all cells within this group as processed
					for (int32_t yy = localY; yy < localY + size; ++yy) {
						for (int32_t xx = localX; xx < localX + size; ++xx) {
							processed[yy * lodLevel + xx] = true;
						}
					}
					cellsRemaining -= size * size;
					instructionCount++;

					Instruction instruction;
					instruction.lodLevel = lodLevel;
					instruction.form.id = formID;
					// Convert back to game cell coords
					instruction.x = cellX + minX;
					instruction.y = cellY + minY;
					instruction.size = size;
					instruction.waterHeight = targetHeight;

					// Add new instruction to list
					instructions.push_back(instruction);

					if (cellsRemaining <= 0)
						break;
				}

				if (cellsRemaining <= 0)
					break;
			}
		}
	}
}

bool WaterCache::TryBuildRuntimeCache(const DiskCache& diskCache, RuntimeCache& cache)
{
	// For LOD4, 8, 16 and 32
	cache.instructions = std::vector<std::vector<std::vector<Instruction>>>(4);

	const auto& hdr = diskCache.header;
	const auto& [minX, minY, maxX, maxY] = hdr.bounds;

	cache.header = hdr;

	int32_t diskReadIndex = 0;
	int32_t skippedInvalidInstructionCount = 0;
	int32_t skippedUnresolvedFormCount = 0;

	for (int32_t lodLevelIdx = 0; lodLevelIdx < 4; ++lodLevelIdx) {
		auto& lodInstructions = cache.instructions[lodLevelIdx];

		const int32_t lodLevel = 1 << (lodLevelIdx + 2);
		int32_t lodMinX, lodMinY, lodMaxX, lodMaxY;
		GetLODCoords(lodLevel, minX, minY, lodMinX, lodMinY);
		GetLODCoords(lodLevel, maxX, maxY, lodMaxX, lodMaxY);
		const int32_t lodWidth = lodMaxX - lodMinX + 1;
		const int32_t lodHeight = lodMaxY - lodMinY + 1;

		lodInstructions.resize(lodWidth * lodHeight);

		while (diskReadIndex < diskCache.instructions.size()) {
			auto instruction = diskCache.instructions[diskReadIndex];
			if (instruction.lodLevel != static_cast<uint32_t>(lodLevel))
				break;

			if (!IsValidCellHeight(instruction.waterHeight)) {
				// Keep old or malformed disk caches from restoring bad water tiles
				skippedInvalidInstructionCount++;
				diskReadIndex++;
				continue;
			}

			int32_t lodX, lodY;
			GetLODCoords(lodLevel, instruction.x, instruction.y, lodX, lodY);
			lodX -= lodMinX;
			lodY -= lodMinY;

			if (lodX < 0 || lodY < 0 || lodX >= lodWidth || lodY >= lodHeight) {
				logger::warn("[Unified Water] [Cache] Skipping out-of-bounds LOD{} instruction at {},{}", lodLevel, instruction.x, instruction.y);
				diskReadIndex++;
				continue;
			}

			instruction.form.ptr = LookupWaterForm(instruction.form.id);
			if (!instruction.form.ptr) {
				if (!skippedUnresolvedFormCount) {
					logger::warn("[Unified Water] [Cache] Failed to load WaterForm {:08X} at LOD{} cell {},{} - skipping instruction",
						instruction.form.id, lodLevel, instruction.x, instruction.y);
				}
				skippedUnresolvedFormCount++;
				diskReadIndex++;
				continue;
			}

			if (!instruction.form.ptr->IsInitialized()) {
				logger::warn("[Unified Water] [Cache] WaterForm {:08X} is not initialized", instruction.form.id);
				instruction.form.ptr->InitItemImpl();
			}

			lodInstructions[lodY * lodWidth + lodX].push_back(instruction);

			diskReadIndex++;
		}
	}

	if (skippedInvalidInstructionCount) {
		logger::debug("[Unified Water] [Cache] Skipped {} cached instructions with invalid water heights", skippedInvalidInstructionCount);
	}

	if (skippedUnresolvedFormCount > 1) {
		logger::warn("[Unified Water] [Cache] Skipped {} cached instructions with unresolvable water forms", skippedUnresolvedFormCount);
	}

	return true;
}

std::vector<RE::TESWorldSpace*> WaterCache::GetValidWorldSpaces()
{
	auto worldSpaces = RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESWorldSpace>();
	auto validWorldSpaces = std::vector<RE::TESWorldSpace*>();
	auto editorIDs = std::unordered_set<std::string>();

	for (auto& worldSpace : worldSpaces) {
		while (worldSpace->parentWorld && worldSpace->parentUseFlags.all(RE::TESWorldSpace::ParentUseFlag::kUseWaterData))
			worldSpace = worldSpace->parentWorld;

		if (!worldSpace->worldWater)
			continue;

		if (std::ranges::find(validWorldSpaces, worldSpace) != validWorldSpaces.end())
			continue;

		// Caches are keyed by EditorID, and mods reuse them: two worldspaces sharing one would overwrite each other's files
		if (const auto editorID = worldSpace->GetFormEditorID(); editorID && *editorID && !editorIDs.emplace(editorID).second)
			continue;

		validWorldSpaces.push_back(worldSpace);
	}

	return validWorldSpaces;
}

void WaterCache::GetLODCoords(const int32_t lodLevel, const int32_t x, const int32_t y, int32_t& outX, int32_t& outY)
{
	outX = x >= 0 ? x / lodLevel : (x - (lodLevel - 1)) / lodLevel;
	outY = y >= 0 ? y / lodLevel : (y - (lodLevel - 1)) / lodLevel;
}

bool WaterCache::TryGetCellData(RE::TESWorldSpace* worldSpace, RE::TESFileArray* files, const int32_t x, const int32_t y, RE::FormID& outFormID, float& outWaterHeight, float& outLandHeight, bool resolveFormID)
{
	outFormID = 0;
	outWaterHeight = FLT_MAX;
	outLandHeight = FLT_MAX;

	const auto size = static_cast<int32_t>(files->size());
	const auto arrayData = files->data();

	int32_t fileIndex = size - 1;
	RE::TESFile* file = arrayData[fileIndex]->Duplicate();
	bool foundWaterData = false;
	bool foundLandData = false;

	// Search through the files in reverse load order to find the cell and read the water height and waterForm FormID
	do {
		if (file && file->SeekCell(worldSpace, x, y)) {
			ReadWaterData(file, outWaterHeight, outFormID);
			foundWaterData = true;
			break;
		}
		file = --fileIndex >= 0 ? arrayData[fileIndex]->Duplicate() : nullptr;
	} while (fileIndex >= 0);

	if (resolveFormID && outFormID)
		outFormID = file->GetRuntimeFormID(outFormID);

	// Continue searching from the previous file to find the original record for the cell, this always has the landscape data - extract land height
	do {
		if (file && file->SeekCell(worldSpace, x, y) && file->SeekLandscapeForCurrentCell()) {
			ReadMinLandHeightData(file, outLandHeight);
			foundLandData = true;
			break;
		}
		file = --fileIndex >= 0 ? arrayData[fileIndex]->Duplicate() : nullptr;
	} while (fileIndex >= 0);

	if (!foundWaterData || !foundLandData) {
		outFormID = 0;
		outWaterHeight = FLT_MIN;
		outLandHeight = 0;
		return false;
	}

	if (outWaterHeight == FLT_MAX)
		outWaterHeight = worldSpace->defaultWaterHeight;
	if (outLandHeight == FLT_MAX)
		outLandHeight = worldSpace->defaultLandHeight;

	return foundWaterData && foundLandData;
}

void WaterCache::ReadWaterData(RE::TESFile* file, float& waterHeight, RE::FormID& formID)
{
	if (!file->SeekNextSubrecordType(Util::FCC("XCLW")))
		return;

	file->ReadData(&waterHeight, 4);
	if (file->isBigEndian)
		waterHeight = std::bit_cast<float>(_byteswap_ulong(std::bit_cast<uint32_t>(waterHeight)));

	if (!file->SeekNextSubrecordType(Util::FCC("XCWT")))
		return;

	file->ReadData(&formID, 4);
	if (file->isBigEndian)
		formID = _byteswap_ulong(formID);
}

void WaterCache::ReadMinLandHeightData(RE::TESFile* file, float& minHeight)
{
	if (!file->SeekNextSubrecordType(Util::FCC("VHGT")))
		return;

	struct VHGTData
	{
		float offset;
		int8_t deltas[1089];
		byte padding[3];
	};
	VHGTData data;

	file->ReadData(&data, 1096);

	if (file->isBigEndian)
		data.offset = std::bit_cast<float>(_byteswap_ulong(std::bit_cast<std::uint32_t>(data.offset)));

	GetMinLandHeight(&data, minHeight);
}

void WaterCache::GetMinLandHeight(const void* vhgt, float& minHeight)
{
	float offset;
	std::memcpy(&offset, vhgt, sizeof(offset));
	const auto* deltas = reinterpret_cast<const int8_t*>(vhgt) + sizeof(offset);

	for (size_t y = 0; y < 33; y++) {
		float rowOffset = 0;
		offset += static_cast<float>(deltas[y * 33]);

		for (size_t x = 0; x < 33; x++) {
			const size_t index = y * 33 + x;

			if (x != 0)
				rowOffset += static_cast<float>(deltas[index]);

			const float height = (rowOffset + offset) * 8.0f;
			minHeight = std::min(height, minHeight);
		}
	}
}

template <typename T>
bool WaterCache::TryWriteCacheToFile(const std::string& name, const WorldSpaceHeader& header, const std::vector<T>& vec)
{
	namespace fs = std::filesystem;
	const fs::path path = Util::PathHelpers::GetUnifiedWaterCachePath() / name;

	const std::span headerBytes(reinterpret_cast<const char*>(&header), sizeof(header));
	const std::span dataBytes(reinterpret_cast<const char*>(vec.data()), vec.size() * sizeof(T));
	if (!Util::FileHelpers::WriteFileAtomic(path, { headerBytes, dataBytes })) {
		logger::error("[Unified Water] [Cache] Failed to write cache '{}'", path.string());
		return false;
	}
	return true;
}

template <typename T>
bool WaterCache::TryReadCacheFromFile(const std::string& name, WorldSpaceHeader& header, std::vector<T>& vec)
{
	namespace fs = std::filesystem;
	const fs::path path = Util::PathHelpers::GetUnifiedWaterCachePath() / name;

	std::vector<char> bytes;
	if (const auto error = Util::FileHelpers::ReadWholeFile(path, bytes); error != 0) {
		if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
			logger::error("[Unified Water] [Cache] Failed to open '{}' for reading (error {})", path.string(), error);
		return false;
	}

	if (bytes.size() < sizeof(header)) {
		logger::error("[Unified Water] [Cache] Invalid or corrupt header for '{}'", path.string());
		return false;
	}

	std::memcpy(&header, bytes.data(), sizeof(header));
	if (header.label != Util::FCC("WTCH")) {
		logger::error("[Unified Water] [Cache] Invalid or corrupt header for '{}'", path.string());
		return false;
	}

	if (header.dataCount < 0 || static_cast<size_t>(header.dataCount) > (bytes.size() - sizeof(header)) / sizeof(T)) {
		logger::error("[Unified Water] [Cache] Payload size mismatch for '{}'", path.string());
		return false;
	}

	vec.resize(header.dataCount);
	if (!vec.empty())
		std::memcpy(vec.data(), bytes.data() + sizeof(header), vec.size() * sizeof(T));
	return true;
}
