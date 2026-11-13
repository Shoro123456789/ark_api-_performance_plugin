// PerfGuard - an AsaApi plugin that cuts avoidable work on an ARK: Survival Ascended server.
//
// Features (all configurable in ArkApi/Plugins/PerfGuard/config.json):
//   1. Tick monitor       - measures real frame times, logs avg / p99 / worst and actor counts.
//   2. Wild overspawn cap - learns the map's normal wild population after a restart and trims only the
//                           excess that builds up over uptime, far away from players. Never touches tames.
//   3. Stale cleanup      - removes long-dead wild corpses and stuck dropped items far from players.
//   4. Scheduled restart  - broadcasts warnings, saves, then exits so the panel restarts the server.
//   5. Engine cvars       - applies a list of console variables at startup, for measured experiments.
//   6. Profiler           - times the main per-frame game functions and explains every hitch.
//   7. Distance throttle  - updates wild dinos far from every player only every Nth frame.
//   8. Idle mode          - lowers the server frame rate while nobody is online.
//   9. Empty maintenance  - forces garbage collection only while the server is empty.
//  10. Restart window     - restarts when the server is empty inside a time window.
//  11. (removed) Log filter - the PrintString hook crashed on ARK 97.21; filter the console in the startup command instead.
//  12. Game thread        - optional priority and CPU pinning for the main game thread.
//  13. Experimental threading - runs chosen wild-dino ticks on worker threads (bRunOnAnyThread). Expect crashes.
//
// Scans read dino fields through offsets resolved once and verified against AsaApi's accessors ("fast dino access"),
// because per-field name lookups made each 70,000-dino scan freeze the server for most of a second under FEX.
//
// Everything runs on the game thread (AsaApi tick/timer/RCON callbacks), so no locking is needed.

#include <API/ARK/Ark.h>
#include <windows.h>
#include <json.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <unordered_set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef PROJECT_NAME
#define PROJECT_NAME "PerfGuard"
#endif

namespace PerfGuard
{
	using json = nlohmann::json;
	using Clock = std::chrono::steady_clock;

	constexpr int kTamedTeamMin = 50000; // ARK convention: player/tribe teams start at 50000, wild teams are below

	// ------------------------------------------------------------------------------------------------ config

	struct CVarEntry
	{
		std::string command;
		bool enabled = false;
	};

	struct Config
	{
		// monitor
		bool monitorEnabled = true;
		int reportIntervalSeconds = 300;
		bool countActorsInReport = true;
		double hitchLogMs = 500.0;

		// wild cap
		bool capEnabled = true;
		bool capDryRun = true;   // safe first run: log only
		int maxWildDinos = 0;              // 0 = automatic baseline
		int autoBaselineStartMinutes = 10; // start sampling the population this long after server start
		int autoBaselineEndMinutes = 30;   // stop sampling here; baseline = highest count seen in the window
		int autoHeadroomPercent = 20;      // cap = baseline * (1 + headroom)
		int perSpeciesMax = 0;             // 0 = off
		int capCheckIntervalSeconds = 600;
		double minDistanceFromPlayers = 20000.0; // Unreal units (cm). 300 = one foundation
		int maxRemovalsPerPass = 150;
		std::vector<std::string> excludeSpecies;

		// cleanup
		bool cleanupEnabled = true;
		bool cleanupDryRun = true; // safe first run: log only
		int cleanupIntervalSeconds = 300;
		int corpseMaxAgeMinutes = 20;
		int droppedItemMaxAgeMinutes = 60;
		bool includePlayerDroppedItems = false;
		double cleanupMinDistanceFromPlayers = 15000.0;
		int maxCleanupPerPass = 200;

		// restart
		bool restartEnabled = false;
		std::vector<std::string> restartTimes; // "HH:MM", server local clock
		std::vector<int> warningMinutes{30, 15, 10, 5, 1};
		int saveToExitDelaySeconds = 30;

		// cvars
		bool cvarsEnabled = true;
		int cvarsReapplyMinutes = 0; // 0 = only at startup
		std::vector<CVarEntry> cvars;

		// profiler
		bool profilerEnabled = true;

		// distance throttle
		bool throttleEnabled = false;
		double tier1Distance = 10000.0;  // 100 m
		int tier1EveryNFrames = 3;
		double tier2Distance = 25000.0;  // 250 m
		int tier2EveryNFrames = 15;
		int throttleScanSeconds = 3;
		float maxAccumulatedSeconds = 1.0f;
		bool throttleActorTick = true;
		bool throttleAI = true;
		bool throttleMovement = true;
		bool throttleAnimation = true;

		// idle mode
		bool idleEnabled = true;
		int idleEmptyForSeconds = 120;
		int idleMaxFPS = 10;
		int activeMaxFPS = 30;

		// empty-server maintenance
		bool maintEnabled = true;
		int maintEmptyForMinutes = 5;
		int maintMinMinutesBetweenGC = 15;

		// restart window (part of ScheduledRestart)
		bool windowEnabled = false;
		std::string windowStart = "03:00";
		std::string windowEnd = "07:00";
		int windowEmptyForMinutes = 5;

		// game thread
		int threadPriority = 0;      // 0 = leave, 1 = above normal, 2 = highest, 3 = time critical
		uint64 threadAffinityMask = 0; // 0 = leave

		// Blueprint profiler
		bool bpEnabled = true;
		int bpSampleEvery = 16;
		int bpTopN = 15;

		// experimental threading
		bool mtEnabled = false;
		bool mtDinoActorTick = true;
		bool mtDinoController = true;
		bool mtDinoMovement = false;
		bool mtDinoMesh = false;
		bool mtIncludeTamed = false;
		double mtOnlyBeyondDistance = 0.0;
		int mtScanSeconds = 10;

		// log filter
		bool logFilterEnabled = true;
		std::vector<std::string> logBlockContains{"CS Cast Failed"};
	};

	Config g_cfg;

	std::string ConfigDir() { return AsaApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/" PROJECT_NAME; }
	std::string ConfigPath() { return ConfigDir() + "/config.json"; }

	template <typename T>
	void Read(const json& j, const char* key, T& out)
	{
		if (j.contains(key) && !j[key].is_null())
			out = j[key].get<T>();
	}

	json DefaultConfigJson()
	{
		return json::parse(R"JSON(
{
  "Monitor": {
    "Enabled": true,
    "ReportIntervalSeconds": 300,
    "CountActorsInReport": true,
    "HitchLogMs": 500
  },
  "WildDinoCap": {
    "Enabled": true,
    "DryRun": true,
    "MaxWildDinos": 0,
    "AutoBaselineStartMinutes": 10,
    "AutoBaselineEndMinutes": 30,
    "AutoHeadroomPercent": 20,
    "PerSpeciesMax": 0,
    "CheckIntervalSeconds": 600,
    "MinDistanceFromPlayers": 20000,
    "MaxRemovalsPerPass": 150,
    "ExcludeSpecies": []
  },
  "Cleanup": {
    "Enabled": true,
    "DryRun": true,
    "IntervalSeconds": 300,
    "CorpseMaxAgeMinutes": 20,
    "DroppedItemMaxAgeMinutes": 60,
    "IncludePlayerDroppedItems": false,
    "MinDistanceFromPlayers": 15000,
    "MaxPerPass": 200
  },
  "ScheduledRestart": {
    "Enabled": false,
    "Times": ["05:00"],
    "WarningMinutes": [30, 15, 10, 5, 1],
    "SaveToExitDelaySeconds": 30,
    "WindowEnabled": false,
    "WindowStart": "03:00",
    "WindowEnd": "07:00",
    "WindowEmptyForMinutes": 5
  },
  "CVars": {
    "Enabled": true,
    "ReapplyMinutes": 0,
    "List": [
      { "Command": "a.URO.Enable 1",                     "Enabled": true,  "Note": "Animation update-rate optimisation (usually already on)" },
      { "Command": "a.ParallelAnimUpdate 1",             "Enabled": true,  "Note": "Run animation update on worker threads instead of the game thread" },
      { "Command": "a.ParallelAnimEvaluation 1",         "Enabled": true,  "Note": "Run animation evaluation on worker threads" },
      { "Command": "a.ParallelAnimInterpolation 1",      "Enabled": true,  "Note": "Run animation interpolation on worker threads" },
      { "Command": "net.UseAdaptiveNetUpdateFrequency 1","Enabled": false, "Note": "EXPERIMENTAL: replicate idle actors less often" },
      { "Command": "tick.AllowAsyncTickDispatch 1",      "Enabled": false, "Note": "EXPERIMENTAL: dispatch tick tasks from worker threads" },
      { "Command": "tick.AllowAsyncTickCleanup 1",       "Enabled": false, "Note": "EXPERIMENTAL: clean up tick tasks on worker threads" },
      { "Command": "gc.AllowIncrementalReachability 1",  "Enabled": false, "Note": "EXPERIMENTAL: spread GC marking over frames. Conflicts with PerformaceGC GC settings" },
      { "Command": "gc.IncrementalReachabilityTimeLimit 0.005", "Enabled": false, "Note": "EXPERIMENTAL: 5 ms GC budget per frame" },
      { "Command": "gc.AllowIncrementalGather 1",        "Enabled": false, "Note": "EXPERIMENTAL: spread GC gather over frames" }
    ]
  },
  "Profiler": {
    "Enabled": true
  },
  "DistanceThrottle": {
    "Enabled": false,
    "Tier1Distance": 10000,
    "Tier1EveryNFrames": 3,
    "Tier2Distance": 25000,
    "Tier2EveryNFrames": 15,
    "ScanIntervalSeconds": 3,
    "MaxAccumulatedSeconds": 1.0,
    "ThrottleActorTick": true,
    "ThrottleAI": true,
    "ThrottleMovement": true,
    "ThrottleAnimation": true
  },
  "IdleMode": {
    "Enabled": true,
    "EmptyForSeconds": 120,
    "IdleMaxFPS": 10,
    "ActiveMaxFPS": 30
  },
  "EmptyServerMaintenance": {
    "Enabled": true,
    "EmptyForMinutes": 5,
    "MinMinutesBetweenGC": 15
  },
  "GameThread": {
    "Priority": 0,
    "AffinityMask": 0
  },
  "BlueprintProfiler": {
    "Enabled": true,
    "SampleEvery": 16,
    "TopN": 15
  },
  "ExperimentalThreading": {
    "Enabled": false,
    "DinoActorTick": true,
    "DinoAIControllerTick": true,
    "DinoMovement": false,
    "DinoMesh": false,
    "IncludeTamed": false,
    "OnlyBeyondDistance": 0,
    "ScanIntervalSeconds": 10
  }
}
)JSON");
	}

	void ApplyConfigJson(const json& root)
	{
		Config c;
		if (root.contains("Monitor"))
		{
			const auto& j = root["Monitor"];
			Read(j, "Enabled", c.monitorEnabled);
			Read(j, "ReportIntervalSeconds", c.reportIntervalSeconds);
			Read(j, "CountActorsInReport", c.countActorsInReport);
			Read(j, "HitchLogMs", c.hitchLogMs);
		}
		if (root.contains("WildDinoCap"))
		{
			const auto& j = root["WildDinoCap"];
			Read(j, "Enabled", c.capEnabled);
			Read(j, "DryRun", c.capDryRun);
			Read(j, "MaxWildDinos", c.maxWildDinos);
			Read(j, "AutoBaselineStartMinutes", c.autoBaselineStartMinutes);
			Read(j, "AutoBaselineEndMinutes", c.autoBaselineEndMinutes);
			Read(j, "AutoHeadroomPercent", c.autoHeadroomPercent);
			Read(j, "PerSpeciesMax", c.perSpeciesMax);
			Read(j, "CheckIntervalSeconds", c.capCheckIntervalSeconds);
			Read(j, "MinDistanceFromPlayers", c.minDistanceFromPlayers);
			Read(j, "MaxRemovalsPerPass", c.maxRemovalsPerPass);
			Read(j, "ExcludeSpecies", c.excludeSpecies);
		}
		if (root.contains("Cleanup"))
		{
			const auto& j = root["Cleanup"];
			Read(j, "Enabled", c.cleanupEnabled);
			Read(j, "DryRun", c.cleanupDryRun);
			Read(j, "IntervalSeconds", c.cleanupIntervalSeconds);
			Read(j, "CorpseMaxAgeMinutes", c.corpseMaxAgeMinutes);
			Read(j, "DroppedItemMaxAgeMinutes", c.droppedItemMaxAgeMinutes);
			Read(j, "IncludePlayerDroppedItems", c.includePlayerDroppedItems);
			Read(j, "MinDistanceFromPlayers", c.cleanupMinDistanceFromPlayers);
			Read(j, "MaxPerPass", c.maxCleanupPerPass);
		}
		if (root.contains("ScheduledRestart"))
		{
			const auto& j = root["ScheduledRestart"];
			Read(j, "Enabled", c.restartEnabled);
			Read(j, "Times", c.restartTimes);
			Read(j, "WarningMinutes", c.warningMinutes);
			Read(j, "SaveToExitDelaySeconds", c.saveToExitDelaySeconds);
			Read(j, "WindowEnabled", c.windowEnabled);
			Read(j, "WindowStart", c.windowStart);
			Read(j, "WindowEnd", c.windowEnd);
			Read(j, "WindowEmptyForMinutes", c.windowEmptyForMinutes);
		}
		if (root.contains("Profiler"))
			Read(root["Profiler"], "Enabled", c.profilerEnabled);
		if (root.contains("DistanceThrottle"))
		{
			const auto& j = root["DistanceThrottle"];
			Read(j, "Enabled", c.throttleEnabled);
			Read(j, "Tier1Distance", c.tier1Distance);
			Read(j, "Tier1EveryNFrames", c.tier1EveryNFrames);
			Read(j, "Tier2Distance", c.tier2Distance);
			Read(j, "Tier2EveryNFrames", c.tier2EveryNFrames);
			Read(j, "ScanIntervalSeconds", c.throttleScanSeconds);
			Read(j, "MaxAccumulatedSeconds", c.maxAccumulatedSeconds);
			Read(j, "ThrottleActorTick", c.throttleActorTick);
			Read(j, "ThrottleAI", c.throttleAI);
			Read(j, "ThrottleMovement", c.throttleMovement);
			Read(j, "ThrottleAnimation", c.throttleAnimation);
		}
		if (root.contains("IdleMode"))
		{
			const auto& j = root["IdleMode"];
			Read(j, "Enabled", c.idleEnabled);
			Read(j, "EmptyForSeconds", c.idleEmptyForSeconds);
			Read(j, "IdleMaxFPS", c.idleMaxFPS);
			Read(j, "ActiveMaxFPS", c.activeMaxFPS);
		}
		if (root.contains("EmptyServerMaintenance"))
		{
			const auto& j = root["EmptyServerMaintenance"];
			Read(j, "Enabled", c.maintEnabled);
			Read(j, "EmptyForMinutes", c.maintEmptyForMinutes);
			Read(j, "MinMinutesBetweenGC", c.maintMinMinutesBetweenGC);
		}
		if (root.contains("GameThread"))
		{
			const auto& j = root["GameThread"];
			Read(j, "Priority", c.threadPriority);
			Read(j, "AffinityMask", c.threadAffinityMask);
		}
		if (root.contains("BlueprintProfiler"))
		{
			const auto& j = root["BlueprintProfiler"];
			Read(j, "Enabled", c.bpEnabled);
			Read(j, "SampleEvery", c.bpSampleEvery);
			Read(j, "TopN", c.bpTopN);
		}
		if (root.contains("ExperimentalThreading"))
		{
			const auto& j = root["ExperimentalThreading"];
			Read(j, "Enabled", c.mtEnabled);
			Read(j, "DinoActorTick", c.mtDinoActorTick);
			Read(j, "DinoAIControllerTick", c.mtDinoController);
			Read(j, "DinoMovement", c.mtDinoMovement);
			Read(j, "DinoMesh", c.mtDinoMesh);
			Read(j, "IncludeTamed", c.mtIncludeTamed);
			Read(j, "OnlyBeyondDistance", c.mtOnlyBeyondDistance);
			Read(j, "ScanIntervalSeconds", c.mtScanSeconds);
		}
		if (root.contains("LogFilter"))
		{
			const auto& j = root["LogFilter"];
			Read(j, "Enabled", c.logFilterEnabled);
			Read(j, "BlockContains", c.logBlockContains);
		}
		if (root.contains("CVars"))
		{
			const auto& j = root["CVars"];
			Read(j, "Enabled", c.cvarsEnabled);
			Read(j, "ReapplyMinutes", c.cvarsReapplyMinutes);
			if (j.contains("List") && j["List"].is_array())
			{
				for (const auto& e : j["List"])
				{
					CVarEntry cv;
					Read(e, "Command", cv.command);
					Read(e, "Enabled", cv.enabled);
					if (!cv.command.empty())
						c.cvars.push_back(cv);
				}
			}
		}

		// sanity limits so a typo can't make the plugin hammer the server
		c.reportIntervalSeconds = (std::max)(30, c.reportIntervalSeconds);
		c.capCheckIntervalSeconds = (std::max)(60, c.capCheckIntervalSeconds);
		c.cleanupIntervalSeconds = (std::max)(60, c.cleanupIntervalSeconds);
		c.autoHeadroomPercent = (std::max)(5, c.autoHeadroomPercent);
		c.autoBaselineEndMinutes = (std::max)(c.autoBaselineStartMinutes + 1, c.autoBaselineEndMinutes);
		c.maxRemovalsPerPass = std::clamp(c.maxRemovalsPerPass, 1, 2000);
		c.maxCleanupPerPass = std::clamp(c.maxCleanupPerPass, 1, 2000);
		c.saveToExitDelaySeconds = std::clamp(c.saveToExitDelaySeconds, 5, 600);
		std::sort(c.warningMinutes.begin(), c.warningMinutes.end(), std::greater<int>());
		c.tier1EveryNFrames = std::clamp(c.tier1EveryNFrames, 1, 120);
		c.tier2EveryNFrames = std::clamp(c.tier2EveryNFrames, 1, 600);
		c.tier2Distance = (std::max)(c.tier2Distance, c.tier1Distance);
		c.throttleScanSeconds = std::clamp(c.throttleScanSeconds, 1, 60);
		c.maxAccumulatedSeconds = std::clamp(c.maxAccumulatedSeconds, 0.05f, 5.0f);
		c.idleMaxFPS = std::clamp(c.idleMaxFPS, 1, 120);
		c.activeMaxFPS = std::clamp(c.activeMaxFPS, 5, 240);
		c.idleEmptyForSeconds = (std::max)(10, c.idleEmptyForSeconds);
		c.maintEmptyForMinutes = (std::max)(1, c.maintEmptyForMinutes);
		c.maintMinMinutesBetweenGC = (std::max)(1, c.maintMinMinutesBetweenGC);
		c.windowEmptyForMinutes = (std::max)(1, c.windowEmptyForMinutes);
		c.threadPriority = std::clamp(c.threadPriority, 0, 3);
		c.mtScanSeconds = std::clamp(c.mtScanSeconds, 2, 300);
		c.bpSampleEvery = std::clamp(c.bpSampleEvery, 1, 1000);
		c.bpTopN = std::clamp(c.bpTopN, 1, 50);

		g_cfg = c;
	}

	std::string LoadConfig()
	{
		const std::string path = ConfigPath();
		std::ifstream in(path);
		if (!in.good())
		{
			const json def = DefaultConfigJson();
			std::ofstream out(path);
			if (out.good())
				out << def.dump(2);
			ApplyConfigJson(def);
			return "config.json not found, wrote defaults to " + path;
		}

		try
		{
			json root;
			in >> root;
			ApplyConfigJson(root);
			return "config loaded from " + path;
		}
		catch (const std::exception& e)
		{
			ApplyConfigJson(DefaultConfigJson());
			return std::string("config.json is invalid (") + e.what() + "), using built-in defaults";
		}
	}

	// ------------------------------------------------------------------------------------------------ helpers

	UWorld* World()
	{
		if (AsaApi::GetApiUtils().GetStatus() != AsaApi::ServerStatus::Ready)
			return nullptr;
		return AsaApi::GetApiUtils().GetWorld();
	}

	std::string ShortClassName(UObjectBase* obj)
	{
		// "Blueprint'/Game/.../Dodo_Character_BP.Dodo_Character_BP'" -> "Dodo_Character_BP"
		std::string bp = AsaApi::GetApiUtils().GetBlueprint(obj).ToString();
		const auto dot = bp.find_last_of('.');
		if (dot != std::string::npos)
			bp = bp.substr(dot + 1);
		bp.erase(std::remove(bp.begin(), bp.end(), '\''), bp.end());
		return bp;
	}

	std::string Lower(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
		return s;
	}

	std::vector<FVector> LivingPlayerPositions(UWorld* world)
	{
		std::vector<FVector> out;
		for (TWeakObjectPtr<APlayerController> weak : world->PlayerControllerListField())
		{
			APlayerController* pc = weak.Get();
			if (pc == nullptr || pc->PawnField() == nullptr)
				continue;
			out.push_back(pc->PawnField()->GetLocation());
		}
		return out;
	}

	double NearestPlayerDistance(const FVector& at, const std::vector<FVector>& players)
	{
		double best = 1.0e12;
		for (const FVector& p : players)
			best = (std::min)(best, static_cast<double>(FVector::Distance(at, p)));
		return best;
	}

	template <typename T>
	TArray<AActor*> AllActorsOf(UWorld* world)
	{
		TArray<AActor*> actors;
		UClass* cls = T::StaticClass();
		if (world != nullptr && cls != nullptr)
			UGameplayStatics::GetAllActorsOfClass(world, TSubclassOf<AActor>(cls), &actors);
		return actors;
	}

	struct DinoLayout;
	extern bool g_fastActors; // set once fast dino access is verified
	bool g_fastActors = false;
	bool IsUsable(AActor* a) { return a != nullptr && (g_fastActors || !a->IsPendingKillPending()); }

	void Broadcast(const std::wstring& msg)
	{
		AsaApi::GetApiUtils().SendNotificationToAll(FColorList::Yellow, 1.3f, 12.0f, nullptr, msg.c_str());
		AsaApi::GetApiUtils().SendChatMessageToAll(FString("Server"), msg.c_str());
	}

	// ------------------------------------------------------------------------------------------------ fast field access
	//
	// Every AsaApi field accessor looks the field up by name in a hash map, and every NativeCall does the same for the
	// function. With ~70,000 dinos and FEX emulation that made one scan take most of a second. Field offsets never change
	// while the server runs, so PerfGuard resolves them once and then reads memory directly. Bit positions come from
	// AsaApi's offsets cache (built from the game's debug symbols). The fast path is verified against the slow accessors
	// on real dinos first; any mismatch keeps the slow path.

	struct BitInfo
	{
		uint64 offset = 0;
		uint32 bit = 0;
		bool found = false;
	};

	inline uint8* BitByte(void* obj, const BitInfo& b) { return reinterpret_cast<uint8*>(obj) + b.offset + b.bit / 8; }
	inline bool GetBit(void* obj, const BitInfo& b) { return (*BitByte(obj, b) >> (b.bit % 8)) & 1; }
	inline void SetBit(void* obj, const BitInfo& b, bool v)
	{
		uint8* p = BitByte(obj, b);
		const uint8 m = static_cast<uint8>(1u << (b.bit % 8));
		*p = v ? static_cast<uint8>(*p | m) : static_cast<uint8>(*p & ~m);
	}

	bool ReadBitfieldCache(const std::filesystem::path& file, std::map<std::string, BitInfo>& wanted)
	{
		std::ifstream in(file, std::ios::binary);
		if (!in.good())
			return false;
		struct RawBitField
		{
			uint64 offset;
			uint32 bit_position;
			uint64 num_bits;
			uint64 length;
		};
		static_assert(sizeof(RawBitField) == 32, "BitField layout must match AsaApi's cache format");
		size_t remaining = 0;
		for (const auto& [k, v] : wanted)
			remaining += v.found ? 0 : 1;
		while (remaining > 0)
		{
			size_t keySize = 0;
			if (!in.read(reinterpret_cast<char*>(&keySize), sizeof(keySize)) || keySize == 0 || keySize > 4096)
				break;
			std::string key(keySize, '\0');
			RawBitField raw{};
			if (!in.read(key.data(), static_cast<std::streamsize>(keySize)) ||
				!in.read(reinterpret_cast<char*>(&raw), sizeof(raw)))
				break;
			auto it = wanted.find(key);
			if (it != wanted.end() && !it->second.found && raw.num_bits == 1 && raw.bit_position < 64 && raw.offset < 0x4000)
			{
				it->second = BitInfo{raw.offset, raw.bit_position, true};
				--remaining;
			}
		}
		return remaining == 0;
	}

	// newest AsaApi bitfield cache first
	std::vector<std::filesystem::path> BitfieldCacheFiles()
	{
		namespace fs = std::filesystem;
		const fs::path root = fs::path(AsaApi::Tools::GetCurrentDir()) / "ArkApi" / "Cache";
		std::error_code ec;
		std::vector<fs::path> files;
		for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
			if (it->is_regular_file(ec) && it->path().filename() == "cached_bitfields.cache")
				files.push_back(it->path());
		std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
			std::error_code e1, e2;
			return fs::last_write_time(a, e1) > fs::last_write_time(b, e2);
		});
		return files;
	}

	bool LoadBitsFromCache(std::map<std::string, BitInfo>& wanted, std::string* fromFile = nullptr)
	{
		for (const auto& f : BitfieldCacheFiles())
		{
			auto copy = wanted;
			if (ReadBitfieldCache(f, copy))
			{
				wanted = copy;
				if (fromFile)
					*fromFile = f.string();
				return true;
			}
		}
		return false;
	}

	struct DinoLayout
	{
		bool ready = false;  // fast path verified and active
		bool failed = false; // verification failed: slow path forever
		int attempts = 0;
		ptrdiff_t team = 0, tamingTeam = 0, controller = 0, root = 0, classPriv = 0, relLoc = 0;
		BitInfo dead, boss;
	};
	DinoLayout g_dl;
	std::unordered_map<UClass*, std::string> g_speciesCache;

	template <typename T>
	inline T& At(void* base, ptrdiff_t off) { return *reinterpret_cast<T*>(reinterpret_cast<char*>(base) + off); }

	inline int Team(APrimalDinoCharacter* d) { return g_dl.ready ? At<int>(d, g_dl.team) : d->TargetingTeamField(); }
	inline int TamingTeam(APrimalDinoCharacter* d) { return g_dl.ready ? At<int>(d, g_dl.tamingTeam) : d->TamingTeamIDField(); }
	inline bool Dead(APrimalDinoCharacter* d) { return g_dl.ready ? GetBit(d, g_dl.dead) : d->IsDead(); }
	inline bool Boss(APrimalDinoCharacter* d) { return g_dl.ready ? GetBit(d, g_dl.boss) : d->bIsBossDino()(); }
	inline AController* Ctrl(APrimalDinoCharacter* d)
	{
		return g_dl.ready ? At<TObjectPtr<AController>>(d, g_dl.controller).Get() : d->ControllerField().Get();
	}
	inline FVector Loc(APrimalDinoCharacter* d)
	{
		if (g_dl.ready)
			if (USceneComponent* root = At<TObjectPtr<USceneComponent>>(d, g_dl.root).Get())
				return At<FVector>(root, g_dl.relLoc);
		return d->GetLocation();
	}
	const std::string& Species(APrimalDinoCharacter* d)
	{
		UClass* cls = g_dl.ready ? At<UClass*>(d, g_dl.classPriv) : d->ClassPrivateField();
		auto it = g_speciesCache.find(cls);
		if (it != g_speciesCache.end())
			return it->second;
		return g_speciesCache.emplace(cls, ShortClassName(d)).first->second;
	}

	// resolves offsets from real dinos and verifies the fast path against the slow accessors
	void EnsureDinoLayout(const TArray<AActor*>& dinos)
	{
		if (g_dl.ready || g_dl.failed || dinos.Num() == 0 || g_dl.attempts >= 5)
			return;
		++g_dl.attempts;
		APrimalDinoCharacter* first = nullptr;
		USceneComponent* firstRoot = nullptr;
		for (AActor* a : dinos)
		{
			if (a == nullptr || a->IsPendingKillPending())
				continue;
			auto* d = static_cast<APrimalDinoCharacter*>(a);
			if (USceneComponent* r = d->RootComponentField().Get())
			{
				first = d;
				firstRoot = r;
				break;
			}
		}
		if (first == nullptr)
			return;

		DinoLayout l;
		char* b = reinterpret_cast<char*>(first);
		l.team = reinterpret_cast<char*>(&first->TargetingTeamField()) - b;
		l.tamingTeam = reinterpret_cast<char*>(&first->TamingTeamIDField()) - b;
		l.controller = reinterpret_cast<char*>(&first->ControllerField()) - b;
		l.root = reinterpret_cast<char*>(&first->RootComponentField()) - b;
		l.classPriv = reinterpret_cast<char*>(&first->ClassPrivateField()) - b;
		l.relLoc = reinterpret_cast<char*>(&firstRoot->RelativeLocationField()) - reinterpret_cast<char*>(firstRoot);

		std::map<std::string, BitInfo> bits{{"APrimalCharacter.bIsDead", {}}, {"APrimalDinoCharacter.bIsBossDino", {}}};
		if (!LoadBitsFromCache(bits))
		{
			g_dl.failed = true;
			Log::GetLog()->warn("Fast dino access unavailable: bit positions not in AsaApi's cache. Using slow accessors.");
			return;
		}
		l.dead = bits["APrimalCharacter.bIsDead"];
		l.boss = bits["APrimalDinoCharacter.bIsBossDino"];

		// verify on up to 300 live dinos
		int checked = 0, bad = 0;
		for (AActor* a : dinos)
		{
			if (checked >= 300)
				break;
			if (a == nullptr || a->IsPendingKillPending())
				continue;
			auto* d = static_cast<APrimalDinoCharacter*>(a);
			USceneComponent* r = At<TObjectPtr<USceneComponent>>(d, l.root).Get();
			if (r == nullptr || r != d->RootComponentField().Get())
				continue;
			++checked;
			const bool deadOk = GetBit(d, l.dead) == static_cast<bool>(d->bIsDead()());
			const bool bossOk = GetBit(d, l.boss) == static_cast<bool>(d->bIsBossDino()());
			const bool teamOk = At<int>(d, l.team) == d->TargetingTeamField();
			const bool locOk = FVector::Distance(At<FVector>(r, l.relLoc), d->GetLocation()) < 50.0;
			if (!(deadOk && bossOk && teamOk && locOk))
				++bad;
		}
		if (checked < 20 || bad > checked / 100)
		{
			g_dl.failed = bad > 0;
			Log::GetLog()->warn("Fast dino access not enabled ({} checked, {} mismatches). Using slow accessors.", checked, bad);
			return;
		}
		l.ready = true;
		l.attempts = g_dl.attempts;
		g_dl = l;
		g_fastActors = true;
		Log::GetLog()->info("Fast dino access verified on {} dinos ({} mismatches)", checked, bad);
	}

	struct ScanTimer
	{
		const char* what;
		Clock::time_point start = Clock::now();
		size_t* count;
		explicit ScanTimer(const char* w, size_t* n) : what(w), count(n) {}
		~ScanTimer()
		{
			const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
			if (ms >= 50.0)
				Log::GetLog()->info("{} scanned {} actors in {:.0f} ms", what, count ? *count : 0, ms);
		}
	};

	// ------------------------------------------------------------------------------------------------ census

	struct Census
	{
		int wildLiving = 0;
		int wildDead = 0;
		int tamedLiving = 0;
		int tamedDead = 0;
		int droppedItems = 0;
		int players = 0;
		std::map<std::string, int> wildBySpecies;
	};

	Census TakeCensus(UWorld* world, bool bySpecies)
	{
		Census c;
		if (world == nullptr)
			return c;
		c.players = static_cast<int>(LivingPlayerPositions(world).size());

		const TArray<AActor*> dinos = AllActorsOf<APrimalDinoCharacter>(world);
		EnsureDinoLayout(dinos);
		size_t scanCount = static_cast<size_t>(dinos.Num());
		ScanTimer scanTimer("TakeCensus", &scanCount);
		for (AActor* a : dinos)
		{
			if (!IsUsable(a))
				continue;
			auto* dino = static_cast<APrimalDinoCharacter*>(a);
			const bool wild = Team(dino) < kTamedTeamMin;
			const bool dead = Dead(dino);
			if (wild && !dead)
			{
				++c.wildLiving;
				if (bySpecies)
					++c.wildBySpecies[Species(dino)];
			}
			else if (wild)
				++c.wildDead;
			else if (!dead)
				++c.tamedLiving;
			else
				++c.tamedDead;
		}

		for (AActor* a : AllActorsOf<ADroppedItem>(world))
			if (IsUsable(a))
				++c.droppedItems;

		return c;
	}

	std::string TopSpecies(const Census& c, size_t n)
	{
		std::vector<std::pair<std::string, int>> v(c.wildBySpecies.begin(), c.wildBySpecies.end());
		std::sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
		std::ostringstream s;
		for (size_t i = 0; i < v.size() && i < n; ++i)
			s << (i ? ", " : "") << v[i].first << "=" << v[i].second;
		return s.str();
	}

	// ------------------------------------------------------------------------------------------------ tick monitor

	struct TickWindow
	{
		uint64 frames = 0;
		double sumMs = 0.0;
		double maxMs = 0.0;
		uint64 over50 = 0;
		uint64 over100 = 0;
		uint64 over250 = 0;
		std::vector<float> samples;
	};

	TickWindow g_window;
	double g_lastWorldTickMs = 0.0; // set by the UWorld::Tick hook when the profiler runs
	Clock::time_point g_lastTick{};
	bool g_haveLastTick = false;
	Clock::time_point g_lastHitchLog{};
	std::string g_lastReport = "No report yet. The first one is written after ReportIntervalSeconds.";

	void OnTick(float /*delta_seconds*/)
	{
		if (!g_cfg.monitorEnabled)
			return;
		const auto now = Clock::now();
		if (!g_haveLastTick)
		{
			g_lastTick = now;
			g_haveLastTick = true;
			return;
		}
		const double ms = std::chrono::duration<double, std::milli>(now - g_lastTick).count();
		g_lastTick = now;

		g_window.frames++;
		g_window.sumMs += ms;
		g_window.maxMs = (std::max)(g_window.maxMs, ms);
		if (ms > 50.0) g_window.over50++;
		if (ms > 100.0) g_window.over100++;
		if (ms > 250.0) g_window.over250++;
		if (g_window.samples.size() < 200000)
			g_window.samples.push_back(static_cast<float>(ms));

		if (ms >= g_cfg.hitchLogMs && now - g_lastHitchLog > std::chrono::seconds(10))
		{
			g_lastHitchLog = now;
			Log::GetLog()->warn("Hitch: frame took {:.0f} ms (last world tick {:.0f} ms; the rest is outside the world tick, "
				"e.g. garbage collection, saving or networking)", ms, g_lastWorldTickMs);
		}
	}

	std::string BuildReport(bool resetWindow)
	{
		TickWindow w = g_window;
		if (resetWindow)
		{
			g_window = TickWindow{};
			g_window.samples.reserve(w.samples.size());
		}

		std::ostringstream s;
		s.setf(std::ios::fixed);
		s.precision(1);
		if (w.frames == 0)
		{
			s << "Tick: no frames measured yet";
		}
		else
		{
			const double avg = w.sumMs / static_cast<double>(w.frames);
			double p99 = 0.0;
			if (!w.samples.empty())
			{
				const size_t idx = static_cast<size_t>(static_cast<double>(w.samples.size() - 1) * 0.99);
				std::nth_element(w.samples.begin(), w.samples.begin() + static_cast<std::ptrdiff_t>(idx), w.samples.end());
				p99 = w.samples[idx];
			}
			s << "Tick: avg " << avg << " ms (" << (avg > 0 ? 1000.0 / avg : 0.0) << " fps), p99 " << p99
			  << " ms, worst " << w.maxMs << " ms, frames>50ms " << w.over50 << ", >100ms " << w.over100
			  << ", >250ms " << w.over250 << " (" << w.frames << " frames)";
		}

		if (g_cfg.countActorsInReport)
		{
			const Census c = TakeCensus(World(), false);
			s << " | players " << c.players << ", wild " << c.wildLiving << " (+" << c.wildDead << " corpses), tamed "
			  << c.tamedLiving << ", dropped items " << c.droppedItems;
		}
		return s.str();
	}

	// ------------------------------------------------------------------------------------------------ wild cap

	Clock::time_point g_serverReadyAt{};
	bool g_serverReady = false;
	int g_baseline = 0;        // highest wild population seen in the baseline window
	bool g_baselineLocked = false;

	double MinutesSinceReady()
	{
		if (!g_serverReady)
			return 0.0;
		return std::chrono::duration<double>(Clock::now() - g_serverReadyAt).count() / 60.0;
	}

	int CurrentCap()
	{
		if (g_cfg.maxWildDinos > 0)
			return g_cfg.maxWildDinos;
		if (!g_baselineLocked || g_baseline <= 0)
			return 0; // still learning
		return static_cast<int>(static_cast<double>(g_baseline) * (1.0 + g_cfg.autoHeadroomPercent / 100.0));
	}

	bool IsExcludedSpecies(const std::string& species)
	{
		const std::string low = Lower(species);
		for (const auto& ex : g_cfg.excludeSpecies)
			if (!ex.empty() && low.find(Lower(ex)) != std::string::npos)
				return true;
		return false;
	}

	void UpdateBaseline(int wildLiving)
	{
		if (g_cfg.maxWildDinos > 0 || g_baselineLocked)
			return;
		const double mins = MinutesSinceReady();
		if (mins < g_cfg.autoBaselineStartMinutes)
			return;
		g_baseline = (std::max)(g_baseline, wildLiving);
		if (mins >= g_cfg.autoBaselineEndMinutes)
		{
			g_baselineLocked = true;
			Log::GetLog()->info("Wild population baseline locked at {} (cap {} with {}% headroom)", g_baseline, CurrentCap(),
				g_cfg.autoHeadroomPercent);
		}
	}

	struct Candidate
	{
		APrimalDinoCharacter* dino;
		std::string species;
		double distance;
	};

	std::string RunWildCap(bool force)
	{
		UWorld* world = World();
		if (world == nullptr)
			return "Server not ready";

		const std::vector<FVector> players = LivingPlayerPositions(world);
		std::vector<Candidate> eligible;
		std::map<std::string, int> speciesCount;
		int wildLiving = 0;

		const TArray<AActor*> dinos = AllActorsOf<APrimalDinoCharacter>(world);
		EnsureDinoLayout(dinos);
		size_t scanCount = static_cast<size_t>(dinos.Num());
		ScanTimer scanTimer("RunWildCap", &scanCount);
		for (AActor* a : dinos)
		{
			if (!IsUsable(a))
				continue;
			auto* dino = static_cast<APrimalDinoCharacter*>(a);
			if (Team(dino) >= kTamedTeamMin || Dead(dino))
				continue;
			++wildLiving;
			const std::string& species = Species(dino);
			++speciesCount[species];

			if (Boss(dino) || TamingTeam(dino) != 0 || IsExcludedSpecies(species))
				continue;
			const double dist = NearestPlayerDistance(Loc(dino), players);
			if (dist < g_cfg.minDistanceFromPlayers)
				continue;
			eligible.push_back({dino, species, dist});
		}

		UpdateBaseline(wildLiving);
		if (!g_cfg.capEnabled && !force)
			return "Wild cap disabled";

		const int cap = CurrentCap();
		std::sort(eligible.begin(), eligible.end(), [](const Candidate& x, const Candidate& y) { return x.distance > y.distance; });

		std::set<APrimalDinoCharacter*> chosen;
		std::map<std::string, int> removedBySpecies;
		int budget = g_cfg.maxRemovalsPerPass;

		// 1) per-species excess first (catches a single species flooding the map)
		if (g_cfg.perSpeciesMax > 0)
		{
			for (const auto& [species, count] : speciesCount)
			{
				int excess = count - g_cfg.perSpeciesMax;
				for (const Candidate& c : eligible)
				{
					if (excess <= 0 || budget <= 0)
						break;
					if (c.species == species && !chosen.count(c.dino))
					{
						chosen.insert(c.dino);
						++removedBySpecies[species];
						--excess;
						--budget;
					}
				}
			}
		}

		// 2) global excess over the cap, farthest from players first
		int over = cap > 0 ? (wildLiving - static_cast<int>(chosen.size())) - cap : 0;
		for (const Candidate& c : eligible)
		{
			if (over <= 0 || budget <= 0)
				break;
			if (chosen.count(c.dino))
				continue;
			chosen.insert(c.dino);
			++removedBySpecies[c.species];
			--over;
			--budget;
		}

		if (!g_cfg.capDryRun)
			for (APrimalDinoCharacter* d : chosen)
				d->Destroy(false, true);

		std::ostringstream s;
		s << "Wild dinos " << wildLiving << ", cap "
		  << (cap > 0 ? std::to_string(cap) : std::string(g_cfg.maxWildDinos > 0 ? "off" : "learning")) << ", baseline "
		  << g_baseline << (g_baselineLocked ? " (locked)" : "") << ", " << (g_cfg.capDryRun ? "would remove " : "removed ")
		  << chosen.size();
		if (!removedBySpecies.empty())
		{
			s << " [";
			bool first = true;
			for (const auto& [sp, n] : removedBySpecies)
			{
				s << (first ? "" : ", ") << sp << "=" << n;
				first = false;
			}
			s << "]";
		}
		if (!chosen.empty() || force)
			Log::GetLog()->info("{}", s.str());
		return s.str();
	}

	// ------------------------------------------------------------------------------------------------ cleanup

	// identifies one actor instance across passes (pointer + creation time guards against address reuse)
	struct ActorKey
	{
		AActor* ptr;
		long double created;
		bool operator<(const ActorKey& o) const { return ptr != o.ptr ? ptr < o.ptr : created < o.created; }
	};
	std::map<ActorKey, Clock::time_point> g_firstSeenDead;

	std::string RunCleanup(bool force)
	{
		UWorld* world = World();
		if (world == nullptr)
			return "Server not ready";
		if (!g_cfg.cleanupEnabled && !force)
			return "Cleanup disabled";

		const auto now = Clock::now();
		const std::vector<FVector> players = LivingPlayerPositions(world);
		int budget = g_cfg.maxCleanupPerPass;
		int corpses = 0;
		int items = 0;
		std::vector<AActor*> toDestroy;

		// dead WILD dinos only. Tamed corpses can hold saddles and gear, so they are never touched.
		std::map<ActorKey, Clock::time_point> stillDead;
		const TArray<AActor*> dinos = AllActorsOf<APrimalDinoCharacter>(world);
		EnsureDinoLayout(dinos);
		size_t scanCount = static_cast<size_t>(dinos.Num());
		ScanTimer scanTimer("RunCleanup", &scanCount);
		for (AActor* a : dinos)
		{
			if (!IsUsable(a))
				continue;
			auto* dino = static_cast<APrimalDinoCharacter*>(a);
			if (Team(dino) >= kTamedTeamMin || !Dead(dino))
				continue;
			const ActorKey key{a, a->CreationTimeField()};
			auto it = g_firstSeenDead.find(key);
			const auto first = it != g_firstSeenDead.end() ? it->second : now;
			stillDead[key] = first;

			const double ageMin = std::chrono::duration<double>(now - first).count() / 60.0;
			if (budget > 0 && ageMin >= g_cfg.corpseMaxAgeMinutes &&
				NearestPlayerDistance(Loc(dino), players) >= g_cfg.cleanupMinDistanceFromPlayers)
			{
				toDestroy.push_back(a);
				++corpses;
				--budget;
			}
		}
		g_firstSeenDead.swap(stillDead);

		// dropped items that outlived their normal lifetime (typically stuck in stasis). Eggs are never touched.
		UClass* eggClass = ADroppedItemEgg::StaticClass();
		for (AActor* a : AllActorsOf<ADroppedItem>(world))
		{
			if (budget <= 0)
				break;
			if (!IsUsable(a) || (eggClass != nullptr && a->IsA(eggClass)))
				continue;
			auto* item = static_cast<ADroppedItem*>(a);
			if (!g_cfg.includePlayerDroppedItems && item->DroppedByPlayerIDField() != 0)
				continue;
			const double ageMin = static_cast<double>(a->GetGameTimeSinceCreation()) / 60.0;
			if (ageMin < g_cfg.droppedItemMaxAgeMinutes)
				continue;
			if (NearestPlayerDistance(a->GetLocation(), players) < g_cfg.cleanupMinDistanceFromPlayers)
				continue;
			toDestroy.push_back(a);
			++items;
			--budget;
		}

		if (!g_cfg.cleanupDryRun)
			for (AActor* a : toDestroy)
				a->Destroy(false, true);

		std::ostringstream s;
		s << "Cleanup: " << (g_cfg.cleanupDryRun ? "would remove " : "removed ") << corpses << " wild corpses, " << items
		  << " stale dropped items (tracking " << g_firstSeenDead.size() << " corpses)";
		if (corpses + items > 0 || force)
			Log::GetLog()->info("{}", s.str());
		return s.str();
	}

	// ------------------------------------------------------------------------------------------------ cvars

	void RunConsoleCommand(const std::string& cmd)
	{
		UWorld* world = World();
		if (world == nullptr)
			return;
		FString fcmd(cmd);
		UKismetSystemLibrary::ExecuteConsoleCommand(world, &fcmd, nullptr);
	}

	int ApplyCVars()
	{
		if (!g_cfg.cvarsEnabled)
			return 0;
		int applied = 0;
		for (const auto& cv : g_cfg.cvars)
		{
			if (!cv.enabled)
				continue;
			RunConsoleCommand(cv.command);
			Log::GetLog()->info("Applied console variable: {}", cv.command);
			++applied;
		}
		return applied;
	}

	// ------------------------------------------------------------------------------------------------ scheduled restart

	struct RestartState
	{
		bool active = false;          // a countdown is running
		std::time_t exitAt = 0;       // wall-clock time of the save
		std::set<int> warned;         // warning minutes already sent
		bool saved = false;
		std::time_t savedAt = 0;
		std::string firedSlot;        // "YYYY-MM-DD HH:MM" of the schedule slot already handled
	};
	RestartState g_restart;

	std::tm LocalTime(std::time_t t)
	{
		std::tm out{};
		localtime_s(&out, &t);
		return out;
	}

	void AnnounceCountdown()
	{
		const double left = std::difftime(g_restart.exitAt, std::time(nullptr));
		const int mins = (std::max)(0, static_cast<int>(std::ceil(left / 60.0)));
		for (int w : g_cfg.warningMinutes)
			if (w >= mins)
				g_restart.warned.insert(w); // don't repeat warnings for times already covered by this message
		if (mins > 0)
			Broadcast(L"Server restart in " + std::to_wstring(mins) + (mins == 1 ? L" minute." : L" minutes.") +
				L" Find a safe spot.");
	}

	void StartCountdown(int minutes, const std::string& reason)
	{
		g_restart.active = true;
		g_restart.warned.clear();
		g_restart.saved = false;
		g_restart.savedAt = 0;
		g_restart.exitAt = std::time(nullptr) + static_cast<std::time_t>(minutes) * 60;
		Log::GetLog()->info("Restart countdown started: {} minute(s) ({})", minutes, reason);
		AnnounceCountdown();
	}

	void CheckSchedule()
	{
		const std::time_t now = std::time(nullptr);

		if (g_cfg.restartEnabled && !g_restart.active && !g_cfg.restartTimes.empty())
		{
			const int lead = g_cfg.warningMinutes.empty() ? 0 : g_cfg.warningMinutes.front();
			for (const auto& hhmm : g_cfg.restartTimes)
			{
				int h = -1, m = -1;
				const auto colon = hhmm.find(':');
				if (colon == std::string::npos)
					continue;
				try
				{
					h = std::stoi(hhmm.substr(0, colon));
					m = std::stoi(hhmm.substr(colon + 1));
				}
				catch (...)
				{
					continue;
				}
				if (h < 0 || h > 23 || m < 0 || m > 59)
					continue;
				std::tm target = LocalTime(now);
				target.tm_hour = h;
				target.tm_min = m;
				target.tm_sec = 0;
				std::time_t tt = std::mktime(&target);
				if (tt + 60 < now) // already passed today, look at tomorrow
				{
					target.tm_mday += 1;
					tt = std::mktime(&target);
				}
				char slot[32];
				std::strftime(slot, sizeof(slot), "%Y-%m-%d %H:%M", &target);
				const double secondsLeft = std::difftime(tt, now);
				if (secondsLeft <= lead * 60.0 && secondsLeft > 0.0 && g_restart.firedSlot != slot)
				{
					g_restart.firedSlot = slot;
					g_restart.active = true;
					g_restart.exitAt = tt;
					g_restart.warned.clear();
					g_restart.saved = false;
					Log::GetLog()->info("Scheduled restart at {}", slot);
					AnnounceCountdown();
					break;
				}
			}
		}

		if (!g_restart.active)
			return;

		const double left = std::difftime(g_restart.exitAt, now);
		for (int w : g_cfg.warningMinutes)
		{
			if (left <= w * 60.0 && left > (w * 60.0) - 60.0 && !g_restart.warned.count(w))
			{
				g_restart.warned.insert(w);
				Broadcast(L"Server restart in " + std::to_wstring(w) + (w == 1 ? L" minute." : L" minutes.") +
					L" Find a safe spot.");
			}
		}

		if (!g_restart.saved && left <= 0.0)
		{
			g_restart.saved = true;
			g_restart.savedAt = now;
			Broadcast(L"Saving the world and restarting now.");
			if (AShooterGameMode* gm = AsaApi::GetApiUtils().GetShooterGameMode())
				gm->SaveWorld(false, true, false);
			Log::GetLog()->info("Restart: world saved, exiting in {} s", g_cfg.saveToExitDelaySeconds);
		}

		if (g_restart.saved && std::difftime(now, g_restart.savedAt) >= g_cfg.saveToExitDelaySeconds)
		{
			g_restart.active = false;
			Log::GetLog()->info("Restart: exiting process so the panel restarts the server");
			if (UShooterCheatManager* cheat = AsaApi::GetApiUtils().GetCheatManager())
				cheat->DoExit();
			else
				RunConsoleCommand("exit");
		}
	}

	// ------------------------------------------------------------------------------------------------ profiler

	enum Bucket : int
	{
		B_World = 0,   // whole UWorld::Tick (everything below runs inside it)
		B_DinoTick,    // APrimalDinoCharacter::Tick
		B_DinoAI,      // APrimalDinoAIController::Tick
		B_Movement,    // UCharacterMovementComponent::TickComponent (dinos and players)
		B_PathFollow,  // UPathFollowingComponent::TickComponent
		B_Anim,        // USkeletalMeshComponent::TickComponent (server-side animation)
		B_Status,      // UPrimalCharacterStatusComponent::TickStatus (nested inside dino/player tick)
		B_PlayerTick,  // AShooterCharacter::Tick
		B_GameMode,    // AShooterGameMode::Tick
		B_ZoneUpdate,  // ANPCZoneManager::UpdateZone (wild spawn zones)
		B_ZoneCheck,   // ANPCZoneManager::CheckIncreaseNPC (decides whether a zone needs more dinos)
		B_ZoneSpawn,   // ANPCZoneManager::SpawnNPCs
		B_HibWake,     // AHibernationManager::AwakenActorsForPlayers (wakes hibernated actors near players)
		B_Count
	};
	const char* kBucketNames[B_Count] = {"world tick", "dino tick", "dino AI", "movement", "pathfinding", "animation",
		"status (inside dino/player tick)", "player tick", "game mode", "spawn zone update", "spawn zone check",
		"spawn zone spawning", "hibernation wake-up"};

	// atomics: with experimental threading some hooked ticks can run on worker threads
	struct BucketStats
	{
		std::atomic<int64> ticks{0};      // QPC ticks in the current report window
		std::atomic<int64> calls{0};
		std::atomic<int64> frameTicks{0}; // QPC ticks in the current frame (for hitch breakdown)
		void Reset()
		{
			ticks.store(0, std::memory_order_relaxed);
			calls.store(0, std::memory_order_relaxed);
			frameTicks.store(0, std::memory_order_relaxed);
		}
	};
	BucketStats g_prof[B_Count];
	int64 g_profFrames = 0;
	int64 g_qpcFreq = 1;
	Clock::time_point g_lastWorldHitchLog{};
	bool g_tickHooksInstalled = false;
	bool g_printHookInstalled = false;

	inline int64 Qpc()
	{
		LARGE_INTEGER v;
		QueryPerformanceCounter(&v);
		return v.QuadPart;
	}

	inline double TicksToMs(int64 t) { return static_cast<double>(t) * 1000.0 / static_cast<double>(g_qpcFreq); }

	// Sampling profiler: reading the clock is slow under Wine/FEX, so only 1 in kSampleEvery calls is timed and
	// the result is scaled up. Unsampled calls cost one thread-local increment.
	constexpr uint32 kSampleEvery = 32;
	thread_local uint32 t_sampleCounter = 0;

	struct ScopedBucket
	{
		Bucket b;
		int64 start;
		bool on;
		int64 weight;
		// always = time every call (for rare, expensive calls); otherwise sample 1 in kSampleEvery
		explicit ScopedBucket(Bucket bucket, bool always = false)
			: b(bucket), start(0), on(g_cfg.profilerEnabled && (always || (++t_sampleCounter % kSampleEvery) == 0)),
			  weight(always ? 1 : static_cast<int64>(kSampleEvery))
		{
			if (on)
				start = Qpc();
		}
		~ScopedBucket()
		{
			if (!on)
				return;
			const int64 d = (Qpc() - start) * weight;
			g_prof[b].ticks.fetch_add(d, std::memory_order_relaxed);
			g_prof[b].frameTicks.fetch_add(d, std::memory_order_relaxed);
			g_prof[b].calls.fetch_add(weight, std::memory_order_relaxed);
		}
	};

	std::string ProfileSummary(bool reset)
	{
		std::ostringstream s;
		s.setf(std::ios::fixed);
		s.precision(2);
		if (!g_cfg.profilerEnabled || !g_tickHooksInstalled)
			return "Profiler off (enable Profiler or DistanceThrottle and restart the server)";
		if (g_profFrames == 0)
			return "Profiler: no frames yet";
		const double frames = static_cast<double>(g_profFrames);
		const double worldMs = TicksToMs(g_prof[B_World].ticks.load()) / frames;
		s << "Profile (sampled 1 in " << kSampleEvery << " calls), ms per frame over " << g_profFrames << " frames: world tick " << worldMs;
		for (int i = B_DinoTick; i < B_Count; ++i)
		{
			const double ms = TicksToMs(g_prof[i].ticks.load()) / frames;
			s << " | " << kBucketNames[i] << " " << ms;
			if (worldMs > 0.0)
				s << " (" << static_cast<int>(ms * 100.0 / worldMs) << "%, " << (g_prof[i].calls.load() / g_profFrames) << " calls)";
		}
		if (reset)
		{
			for (auto& b : g_prof)
				b.Reset();
			g_profFrames = 0;
		}
		return s.str();
	}

	// ------------------------------------------------------------------------------------------------ distance throttle

	enum Slot : int { S_Actor = 0, S_AI, S_Move, S_Anim, S_Path, S_Count };

	struct ThrottleState
	{
		int every = 1;
		int frame[S_Count]{};
		float acc[S_Count]{};
	};
	std::unordered_map<const void*, ThrottleState> g_throttle; // keyed by dino AND its AI controller
	int g_throttleTier1 = 0;
	int g_throttleTier2 = 0;
	std::atomic<int64> g_throttleSkipped{0};
	std::shared_mutex g_throttleMutex;       // guards g_throttle against worker-thread ticks during a rebuild
	std::atomic<size_t> g_throttleSize{0};
	std::atomic<bool> g_parallelTicks{false}; // true only while experimental threading has ticks on worker threads

	// returns false when this call should be skipped; otherwise dt holds the accumulated delta to pass on
	inline bool ThrottleAllows(const void* key, Slot slot, float& dt)
	{
		if (!g_cfg.throttleEnabled || g_throttleSize.load(std::memory_order_relaxed) == 0 || key == nullptr)
			return true;
		// all hooked ticks run on the game thread unless experimental threading is active, so skip the lock then
		std::shared_lock<std::shared_mutex> lock(g_throttleMutex, std::defer_lock);
		const bool parallel = g_parallelTicks.load(std::memory_order_relaxed);
		if (parallel)
			lock.lock();
		const auto it = g_throttle.find(key);
		if (it == g_throttle.end())
			return true;
		ThrottleState& st = it->second;
		st.acc[slot] += dt;
		if (++st.frame[slot] < st.every)
		{
			if (parallel)
				g_throttleSkipped.fetch_add(1, std::memory_order_relaxed);
			else
				g_throttleSkipped.store(g_throttleSkipped.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
			return false;
		}
		st.frame[slot] = 0;
		dt = (std::min)(st.acc[slot], g_cfg.maxAccumulatedSeconds);
		st.acc[slot] = 0.0f;
		return true;
	}

	void RebuildThrottle()
	{
		if (!g_cfg.throttleEnabled)
		{
			std::unique_lock lock(g_throttleMutex);
			g_throttle.clear();
			g_throttleSize = 0;
			g_throttleTier1 = g_throttleTier2 = 0;
			return;
		}
		UWorld* world = World();
		if (world == nullptr)
			return;

		const std::vector<FVector> players = LivingPlayerPositions(world);
		std::unordered_map<const void*, ThrottleState> next;
		next.reserve(g_throttle.size() + 64);
		int t1 = 0, t2 = 0;

		const TArray<AActor*> dinos = AllActorsOf<APrimalDinoCharacter>(world);
		EnsureDinoLayout(dinos);
		size_t scanCount = static_cast<size_t>(dinos.Num());
		ScanTimer scanTimer("RebuildThrottle", &scanCount);
		for (AActor* a : dinos)
		{
			if (!IsUsable(a))
				continue;
			auto* dino = static_cast<APrimalDinoCharacter*>(a);
			if (Team(dino) >= kTamedTeamMin || Dead(dino) || Boss(dino) || TamingTeam(dino) != 0)
				continue;
			const double dist = NearestPlayerDistance(Loc(dino), players);
			int every = 1;
			if (dist >= g_cfg.tier2Distance)
			{
				every = g_cfg.tier2EveryNFrames;
				++t2;
			}
			else if (dist >= g_cfg.tier1Distance)
			{
				every = g_cfg.tier1EveryNFrames;
				++t1;
			}
			if (every <= 1)
				continue;

			// keep partial accumulators when a dino stays in the same tier
			const auto keep = [&](const void* key) {
				ThrottleState st;
				const auto old = g_throttle.find(key);
				if (old != g_throttle.end() && old->second.every == every)
					st = old->second;
				st.every = every;
				next[key] = st;
			};
			keep(dino);
			if (AController* ctrl = Ctrl(dino))
				keep(ctrl);
		}
		{
			std::unique_lock lock(g_throttleMutex);
			g_throttle.swap(next);
			g_throttleSize = g_throttle.size();
		}
		g_throttleTier1 = t1;
		g_throttleTier2 = t2;
	}

	std::string ThrottleSummary()
	{
		std::ostringstream s;
		if (!g_cfg.throttleEnabled)
			return "Distance throttle off";
		s << "Distance throttle: " << g_throttleTier1 << " wild dinos at 1/" << g_cfg.tier1EveryNFrames << " rate, "
		  << g_throttleTier2 << " at 1/" << g_cfg.tier2EveryNFrames << " rate, " << g_throttleSkipped.load()
		  << " ticks skipped since last report";
		return s.str();
	}

	// ------------------------------------------------------------------------------------------------ hooks

	DECLARE_HOOK(UWorld_Tick, void, UWorld*, DWORD64, float);
	DECLARE_HOOK(APrimalDinoCharacter_Tick, void, APrimalDinoCharacter*, float);
	DECLARE_HOOK(APrimalDinoAIController_Tick, void, APrimalDinoAIController*, float);
	DECLARE_HOOK(UCharacterMovementComponent_TickComponent, void, UCharacterMovementComponent*, float, DWORD64, FActorComponentTickFunction*);
	DECLARE_HOOK(UPathFollowingComponent_TickComponent, void, UPathFollowingComponent*, float, DWORD64, FActorComponentTickFunction*);
	DECLARE_HOOK(USkeletalMeshComponent_TickComponent, void, USkeletalMeshComponent*, float, DWORD64, FActorComponentTickFunction*);
	DECLARE_HOOK(UPrimalCharacterStatusComponent_TickStatus, void, UPrimalCharacterStatusComponent*, float, bool);
	DECLARE_HOOK(AShooterCharacter_Tick, void, AShooterCharacter*, float);
	DECLARE_HOOK(AShooterGameMode_Tick, void, AShooterGameMode*, float);
	DECLARE_HOOK(ANPCZoneManager_UpdateZone, bool, ANPCZoneManager*);
	DECLARE_HOOK(ANPCZoneManager_CheckIncreaseNPC, void, ANPCZoneManager*, bool);
	DECLARE_HOOK(ANPCZoneManager_SpawnNPCs, bool, ANPCZoneManager*);
	DECLARE_HOOK(AHibernationManager_AwakenActorsForPlayers, void, AHibernationManager*, bool);
	DECLARE_HOOK(UObject_ProcessEvent, void, UObject*, UFunction*, void*);

	void Hook_UWorld_Tick(UWorld* world, DWORD64 tickType, float dt)
	{
		if (!g_cfg.profilerEnabled)
		{
			UWorld_Tick_original(world, tickType, dt);
			return;
		}
		for (auto& b : g_prof)
			b.frameTicks.store(0, std::memory_order_relaxed);
		const int64 start = Qpc();
		UWorld_Tick_original(world, tickType, dt);
		const int64 d = Qpc() - start;
		g_prof[B_World].ticks.fetch_add(d, std::memory_order_relaxed);
		g_prof[B_World].calls.fetch_add(1, std::memory_order_relaxed);
		++g_profFrames;
		g_lastWorldTickMs = TicksToMs(d);

		const auto now = Clock::now();
		if (g_lastWorldTickMs >= g_cfg.hitchLogMs && now - g_lastWorldHitchLog > std::chrono::seconds(10))
		{
			g_lastWorldHitchLog = now;
			std::ostringstream s;
			s.setf(std::ios::fixed);
			s.precision(0);
			s << "Hitch inside world tick: " << g_lastWorldTickMs << " ms. Breakdown:";
			for (int i = B_DinoTick; i < B_Count; ++i)
				s << " " << kBucketNames[i] << " " << TicksToMs(g_prof[i].frameTicks.load()) << ",";
			s << " other (network replication, structures, Blueprints, mods) is the remainder";
			Log::GetLog()->warn("{}", s.str());
		}
	}

	void Hook_APrimalDinoCharacter_Tick(APrimalDinoCharacter* self, float dt)
	{
		if (g_cfg.throttleActorTick && !ThrottleAllows(self, S_Actor, dt))
			return;
		ScopedBucket sb(B_DinoTick);
		APrimalDinoCharacter_Tick_original(self, dt);
	}

	void Hook_APrimalDinoAIController_Tick(APrimalDinoAIController* self, float dt)
	{
		if (g_cfg.throttleAI && !ThrottleAllows(self, S_AI, dt))
			return;
		ScopedBucket sb(B_DinoAI);
		APrimalDinoAIController_Tick_original(self, dt);
	}

	void Hook_UCharacterMovementComponent_TickComponent(UCharacterMovementComponent* self, float dt, DWORD64 tickType,
		FActorComponentTickFunction* fn)
	{
		if (g_cfg.throttleMovement && !ThrottleAllows(self->OwnerPrivateField(), S_Move, dt))
			return;
		ScopedBucket sb(B_Movement);
		UCharacterMovementComponent_TickComponent_original(self, dt, tickType, fn);
	}

	void Hook_UPathFollowingComponent_TickComponent(UPathFollowingComponent* self, float dt, DWORD64 tickType,
		FActorComponentTickFunction* fn)
	{
		if (g_cfg.throttleAI && !ThrottleAllows(self->OwnerPrivateField(), S_Path, dt))
			return;
		ScopedBucket sb(B_PathFollow);
		UPathFollowingComponent_TickComponent_original(self, dt, tickType, fn);
	}

	void Hook_USkeletalMeshComponent_TickComponent(USkeletalMeshComponent* self, float dt, DWORD64 tickType,
		FActorComponentTickFunction* fn)
	{
		if (g_cfg.throttleAnimation && !ThrottleAllows(self->OwnerPrivateField(), S_Anim, dt))
			return;
		ScopedBucket sb(B_Anim);
		USkeletalMeshComponent_TickComponent_original(self, dt, tickType, fn);
	}

	void Hook_UPrimalCharacterStatusComponent_TickStatus(UPrimalCharacterStatusComponent* self, float dt, bool force)
	{
		ScopedBucket sb(B_Status);
		UPrimalCharacterStatusComponent_TickStatus_original(self, dt, force);
	}

	void Hook_AShooterCharacter_Tick(AShooterCharacter* self, float dt)
	{
		ScopedBucket sb(B_PlayerTick);
		AShooterCharacter_Tick_original(self, dt);
	}

	void Hook_AShooterGameMode_Tick(AShooterGameMode* self, float dt)
	{
		ScopedBucket sb(B_GameMode, true);
		AShooterGameMode_Tick_original(self, dt);
	}

	bool Hook_ANPCZoneManager_UpdateZone(ANPCZoneManager* self)
	{
		ScopedBucket sb(B_ZoneUpdate, true);
		return ANPCZoneManager_UpdateZone_original(self);
	}

	void Hook_ANPCZoneManager_CheckIncreaseNPC(ANPCZoneManager* self, bool force)
	{
		ScopedBucket sb(B_ZoneCheck, true);
		ANPCZoneManager_CheckIncreaseNPC_original(self, force);
	}

	bool Hook_ANPCZoneManager_SpawnNPCs(ANPCZoneManager* self)
	{
		ScopedBucket sb(B_ZoneSpawn, true);
		return ANPCZoneManager_SpawnNPCs_original(self);
	}

	void Hook_AHibernationManager_AwakenActorsForPlayers(AHibernationManager* self, bool syncLoad)
	{
		ScopedBucket sb(B_HibWake, true);
		AHibernationManager_AwakenActorsForPlayers_original(self, syncLoad);
	}

	// ---- Blueprint profiler
	// Every Blueprint event that native code triggers (ReceiveTick, timers, delegates, mod logic) enters through
	// UObject::ProcessEvent. Only outermost calls are timed, so nested Blueprint calls are counted inside their caller.
	// Mods are Blueprint-only, so the function path shows which mod a cost belongs to.
	struct BpStat
	{
		int64 ticks = 0;
		int64 calls = 0;
	};
	std::mutex g_bpMutex;
	std::unordered_map<UFunction*, BpStat> g_bpStats;
	std::atomic<int64> g_bpTotalTicks{0};
	thread_local int t_peDepth = 0;
	thread_local uint32 t_peSample = 0;

	void Hook_UObject_ProcessEvent(UObject* self, UFunction* fn, void* parms)
	{
		const bool sample = g_cfg.bpEnabled && t_peDepth == 0 && fn != nullptr &&
			(++t_peSample % static_cast<uint32>(g_cfg.bpSampleEvery)) == 0;
		++t_peDepth;
		if (!sample)
		{
			UObject_ProcessEvent_original(self, fn, parms);
			--t_peDepth;
			return;
		}
		const int64 start = Qpc();
		UObject_ProcessEvent_original(self, fn, parms);
		const int64 d = (Qpc() - start) * g_cfg.bpSampleEvery;
		--t_peDepth;
		g_bpTotalTicks.fetch_add(d, std::memory_order_relaxed);
		std::lock_guard lock(g_bpMutex);
		BpStat& st = g_bpStats[fn];
		st.ticks += d;
		st.calls += g_cfg.bpSampleEvery;
	}

	std::string FunctionPath(UFunction* fn)
	{
		if (fn == nullptr)
			return "?";
		FString out;
		fn->GetPathName(nullptr, out);
		std::string p = out.ToString();
		// "/Game/Mods/SomeMod/Blueprints/BP_Thing.BP_Thing_C:ReceiveTick" -> keep it short but keep the mod folder
		if (p.rfind("/Game/", 0) == 0)
			p = p.substr(6);
		return p;
	}

	std::string BlueprintSummary(bool reset)
	{
		if (!g_cfg.bpEnabled || !g_tickHooksInstalled)
			return "Blueprint profiler off";
		std::vector<std::pair<UFunction*, BpStat>> rows;
		{
			std::lock_guard lock(g_bpMutex);
			rows.assign(g_bpStats.begin(), g_bpStats.end());
			if (reset)
				g_bpStats.clear();
		}
		const int64 frames = (std::max)(g_profFrames, static_cast<int64>(1));
		std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.ticks > b.second.ticks; });
		std::ostringstream s;
		s.setf(std::ios::fixed);
		s.precision(2);
		s << "Blueprint events (sampled 1 in " << g_cfg.bpSampleEvery << "), total "
		  << TicksToMs(g_bpTotalTicks.load()) / static_cast<double>(frames) << " ms per frame. Top " << g_cfg.bpTopN << ":";
		for (size_t i = 0; i < rows.size() && i < static_cast<size_t>(g_cfg.bpTopN); ++i)
			s << "\n  " << TicksToMs(rows[i].second.ticks) / static_cast<double>(frames) << " ms/frame, "
			  << rows[i].second.calls / frames << " calls/frame  " << FunctionPath(rows[i].first);
		if (reset)
			g_bpTotalTicks.store(0);
		return s.str();
	}

	int64 g_printBlocked = 0; // log filter disabled: see README (PrintString hook crashed on ARK 97.21)

	struct HookDef
	{
		const char* name;
		LPVOID detour;
		LPVOID* original;
		bool tickGroup; // false = PrintString
	};

	std::vector<HookDef> HookList()
	{
		return {
			{"UWorld.Tick(ELevelTick,float)", reinterpret_cast<LPVOID>(&Hook_UWorld_Tick), reinterpret_cast<LPVOID*>(&UWorld_Tick_original), true},
			{"APrimalDinoCharacter.Tick(float)", reinterpret_cast<LPVOID>(&Hook_APrimalDinoCharacter_Tick),
				reinterpret_cast<LPVOID*>(&APrimalDinoCharacter_Tick_original), true},
			{"APrimalDinoAIController.Tick(float)", reinterpret_cast<LPVOID>(&Hook_APrimalDinoAIController_Tick),
				reinterpret_cast<LPVOID*>(&APrimalDinoAIController_Tick_original), true},
			{"UCharacterMovementComponent.TickComponent(float,ELevelTick,FActorComponentTickFunction*)",
				reinterpret_cast<LPVOID>(&Hook_UCharacterMovementComponent_TickComponent),
				reinterpret_cast<LPVOID*>(&UCharacterMovementComponent_TickComponent_original), true},
			{"UPathFollowingComponent.TickComponent(float,ELevelTick,FActorComponentTickFunction*)",
				reinterpret_cast<LPVOID>(&Hook_UPathFollowingComponent_TickComponent),
				reinterpret_cast<LPVOID*>(&UPathFollowingComponent_TickComponent_original), true},
			{"USkeletalMeshComponent.TickComponent(float,ELevelTick,FActorComponentTickFunction*)",
				reinterpret_cast<LPVOID>(&Hook_USkeletalMeshComponent_TickComponent),
				reinterpret_cast<LPVOID*>(&USkeletalMeshComponent_TickComponent_original), true},
			{"UPrimalCharacterStatusComponent.TickStatus(float,bool)", reinterpret_cast<LPVOID>(&Hook_UPrimalCharacterStatusComponent_TickStatus),
				reinterpret_cast<LPVOID*>(&UPrimalCharacterStatusComponent_TickStatus_original), true},
			{"AShooterCharacter.Tick(float)", reinterpret_cast<LPVOID>(&Hook_AShooterCharacter_Tick),
				reinterpret_cast<LPVOID*>(&AShooterCharacter_Tick_original), true},
			{"ANPCZoneManager.UpdateZone()", reinterpret_cast<LPVOID>(&Hook_ANPCZoneManager_UpdateZone),
				reinterpret_cast<LPVOID*>(&ANPCZoneManager_UpdateZone_original), true},
			{"ANPCZoneManager.CheckIncreaseNPC(bool)", reinterpret_cast<LPVOID>(&Hook_ANPCZoneManager_CheckIncreaseNPC),
				reinterpret_cast<LPVOID*>(&ANPCZoneManager_CheckIncreaseNPC_original), true},
			{"ANPCZoneManager.SpawnNPCs()", reinterpret_cast<LPVOID>(&Hook_ANPCZoneManager_SpawnNPCs),
				reinterpret_cast<LPVOID*>(&ANPCZoneManager_SpawnNPCs_original), true},
			{"AHibernationManager.AwakenActorsForPlayers(bool)", reinterpret_cast<LPVOID>(&Hook_AHibernationManager_AwakenActorsForPlayers),
				reinterpret_cast<LPVOID*>(&AHibernationManager_AwakenActorsForPlayers_original), true},
			{"UObject.ProcessEvent(UFunction*,void*)", reinterpret_cast<LPVOID>(&Hook_UObject_ProcessEvent),
				reinterpret_cast<LPVOID*>(&UObject_ProcessEvent_original), true},
			{"AShooterGameMode.Tick(float)", reinterpret_cast<LPVOID>(&Hook_AShooterGameMode_Tick),
				reinterpret_cast<LPVOID*>(&AShooterGameMode_Tick_original), true},
		};
	}

	void InstallHooks()
	{
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		g_qpcFreq = f.QuadPart > 0 ? f.QuadPart : 1;

		const bool wantTick = g_cfg.profilerEnabled || g_cfg.throttleEnabled || g_cfg.bpEnabled;
		const bool wantPrint = false; // PrintString hook removed: its native signature differs on current ARK builds
		int ok = 0;
		for (const auto& h : HookList())
		{
			if ((h.tickGroup && !wantTick) || (!h.tickGroup && !wantPrint))
				continue;
			if (AsaApi::GetHooks().SetHook(h.name, h.detour, h.original))
				++ok;
			else
				Log::GetLog()->error("Could not hook {}", h.name);
		}
		g_tickHooksInstalled = wantTick;
		g_printHookInstalled = wantPrint;
		Log::GetLog()->info("Installed {} hook(s) (tick hooks {}, log filter {})", ok, wantTick ? "on" : "off",
			wantPrint ? "on" : "off");
	}

	void RemoveHooks()
	{
		for (const auto& h : HookList())
		{
			if ((h.tickGroup && g_tickHooksInstalled) || (!h.tickGroup && g_printHookInstalled))
				AsaApi::GetHooks().DisableHook(h.name, h.detour);
		}
		g_tickHooksInstalled = g_printHookInstalled = false;
	}

	// ------------------------------------------------------------------------------------------------ experimental threading
	//
	// Unreal decides per tick function, every frame, whether it runs on the game thread or on a worker thread
	// (FTickFunction::bRunOnAnyThread). ARK leaves it off for its own ticks. AsaApi's headers don't expose that
	// bit, but AsaApi's offsets cache (built from the game's PDB) records the exact bit position, so we read it
	// from there instead of guessing. Flipping it makes the chosen ticks run in parallel with the game thread.
	// ARK's code was never written for that, so expect crashes: this is a measured experiment, off by default.

	BitInfo g_bitAnyThread;
	BitInfo g_bitCanEverTick;
	std::string g_layoutStatus = "not loaded";

	void LoadTickLayout()
	{
		std::map<std::string, BitInfo> wanted{{"FTickFunction.bRunOnAnyThread", {}}, {"FTickFunction.bCanEverTick", {}}};
		std::string file;
		if (LoadBitsFromCache(wanted, &file))
		{
			g_bitAnyThread = wanted["FTickFunction.bRunOnAnyThread"];
			g_bitCanEverTick = wanted["FTickFunction.bCanEverTick"];
			std::ostringstream s;
			s << "tick layout from " << file << ": bRunOnAnyThread at +" << g_bitAnyThread.offset << " bit "
			  << g_bitAnyThread.bit << ", bCanEverTick at +" << g_bitCanEverTick.offset << " bit " << g_bitCanEverTick.bit;
			g_layoutStatus = s.str();
			Log::GetLog()->info("Experimental threading: {}", g_layoutStatus);
			return;
		}
		g_layoutStatus = BitfieldCacheFiles().empty() ? "no AsaApi bitfield cache found under ArkApi/Cache"
			: "FTickFunction bits not present in AsaApi's cache";
		Log::GetLog()->warn("Experimental threading unavailable: {}", g_layoutStatus);
	}

	std::unordered_map<void*, bool> g_anyThreadFlipped; // tick function -> original bit value
	int g_anyThreadCount = 0;
	bool g_anyThreadAborted = false;

	// collects the tick functions of one dino that the config selects
	void SelectedTickFunctions(APrimalDinoCharacter* dino, std::vector<void*>& out)
	{
		if (g_cfg.mtDinoActorTick)
			out.push_back(&dino->PrimaryActorTickField());
		if (g_cfg.mtDinoController)
			if (AController* ctrl = Ctrl(dino))
				out.push_back(&ctrl->PrimaryActorTickField());
		if (g_cfg.mtDinoMovement)
			if (UCharacterMovementComponent* mv = dino->CharacterMovementField().Get())
				out.push_back(&mv->PrimaryComponentTickField());
		if (g_cfg.mtDinoMesh)
			if (USkeletalMeshComponent* mesh = dino->MeshField().Get())
				out.push_back(&mesh->PrimaryComponentTickField());
	}

	void RestoreAnyThread(const std::unordered_set<void*>* alive)
	{
		for (const auto& [fn, original] : g_anyThreadFlipped)
			if (alive == nullptr || alive->count(fn)) // never touch memory of actors that no longer exist
				SetBit(fn, g_bitAnyThread, original);
		g_anyThreadFlipped.clear();
		g_anyThreadCount = 0;
		g_parallelTicks = false;
	}

	void UpdateAnyThread()
	{
		const bool want = g_cfg.mtEnabled && g_bitAnyThread.found && !g_anyThreadAborted;
		UWorld* world = World();
		if (world == nullptr)
			return;
		if (!want && g_anyThreadFlipped.empty())
			return;

		const std::vector<FVector> players = LivingPlayerPositions(world);
		std::unordered_set<void*> alive;   // every selectable tick function that exists right now
		std::unordered_set<void*> desired; // the ones that should run on worker threads
		std::vector<void*> fns;

		const TArray<AActor*> dinos = AllActorsOf<APrimalDinoCharacter>(world);
		EnsureDinoLayout(dinos);
		size_t scanCount = static_cast<size_t>(dinos.Num());
		ScanTimer scanTimer("UpdateAnyThread", &scanCount);
		for (AActor* a : dinos)
		{
			if (!IsUsable(a))
				continue;
			auto* dino = static_cast<APrimalDinoCharacter*>(a);
			fns.clear();
			SelectedTickFunctions(dino, fns);
			for (void* fn : fns)
				alive.insert(fn);

			if (!want || Dead(dino) || Boss(dino))
				continue;
			const bool wild = Team(dino) < kTamedTeamMin;
			if (!wild && !g_cfg.mtIncludeTamed)
				continue;
			if (g_cfg.mtOnlyBeyondDistance > 0.0 &&
				NearestPlayerDistance(Loc(dino), players) < g_cfg.mtOnlyBeyondDistance)
				continue;
			for (void* fn : fns)
				desired.insert(fn);
		}

		// sanity check: every selected tick function must report bCanEverTick, otherwise the layout is wrong
		if (want && g_bitCanEverTick.found)
		{
			int checked = 0, ok = 0;
			for (void* fn : desired)
			{
				if (++checked > 64)
					break;
				ok += GetBit(fn, g_bitCanEverTick) ? 1 : 0;
			}
			if (checked > 8 && ok * 2 < (std::min)(checked, 64))
			{
				g_anyThreadAborted = true;
				Log::GetLog()->error("Experimental threading aborted: only {}/{} tick functions look valid, layout mismatch",
					ok, (std::min)(checked, 64));
				RestoreAnyThread(&alive);
				return;
			}
		}

		// restore the ones that left the selection (player came close, dino tamed, feature switched off)
		for (auto it = g_anyThreadFlipped.begin(); it != g_anyThreadFlipped.end();)
		{
			if (!alive.count(it->first))
				it = g_anyThreadFlipped.erase(it); // actor gone: forget it, don't touch freed memory
			else if (!desired.count(it->first))
			{
				SetBit(it->first, g_bitAnyThread, it->second);
				it = g_anyThreadFlipped.erase(it);
			}
			else
				++it;
		}
		// flip the new ones
		for (void* fn : desired)
		{
			if (g_anyThreadFlipped.count(fn))
				continue;
			g_anyThreadFlipped[fn] = GetBit(fn, g_bitAnyThread);
			SetBit(fn, g_bitAnyThread, true);
		}
		g_anyThreadCount = static_cast<int>(g_anyThreadFlipped.size());
		g_parallelTicks = g_anyThreadCount > 0;
	}

	std::string ThreadingSummary()
	{
		std::ostringstream s;
		s << "Experimental threading " << (g_cfg.mtEnabled ? "on" : "off");
		if (g_anyThreadAborted)
			s << " (ABORTED: layout check failed)";
		s << ", " << g_anyThreadCount << " tick functions on worker threads; " << g_layoutStatus;
		return s.str();
	}

	// ------------------------------------------------------------------------------------------------ idle mode, maintenance, game thread

	int g_playersOnline = 0;
	int g_emptySeconds = 0;
	bool g_idleActive = false;
	Clock::time_point g_lastMaintGC{};
	bool g_haveMaintGC = false;

	int CountPlayers(UWorld* world)
	{
		if (world == nullptr)
			return 0;
		int n = 0;
		for (TWeakObjectPtr<APlayerController> weak : world->PlayerControllerListField())
			if (weak.Get() != nullptr)
				++n;
		return n;
	}

	void UpdatePresence()
	{
		g_playersOnline = CountPlayers(World());
		g_emptySeconds = g_playersOnline == 0 ? g_emptySeconds + 1 : 0;

		// idle mode
		if (g_cfg.idleEnabled)
		{
			if (!g_idleActive && g_playersOnline == 0 && g_emptySeconds >= g_cfg.idleEmptyForSeconds)
			{
				g_idleActive = true;
				RunConsoleCommand("t.MaxFPS " + std::to_string(g_cfg.idleMaxFPS));
				Log::GetLog()->info("Idle mode on: server empty, frame rate capped at {}", g_cfg.idleMaxFPS);
			}
			else if (g_idleActive && g_playersOnline > 0)
			{
				g_idleActive = false;
				RunConsoleCommand("t.MaxFPS " + std::to_string(g_cfg.activeMaxFPS));
				Log::GetLog()->info("Idle mode off: player online, frame rate back to {}", g_cfg.activeMaxFPS);
			}
		}
		else if (g_idleActive)
		{
			g_idleActive = false;
			RunConsoleCommand("t.MaxFPS " + std::to_string(g_cfg.activeMaxFPS));
		}

		// garbage collection while nobody can notice the freeze
		if (g_cfg.maintEnabled && g_playersOnline == 0 && g_emptySeconds >= g_cfg.maintEmptyForMinutes * 60)
		{
			const auto now = Clock::now();
			if (!g_haveMaintGC || now - g_lastMaintGC >= std::chrono::minutes(g_cfg.maintMinMinutesBetweenGC))
			{
				g_lastMaintGC = now;
				g_haveMaintGC = true;
				if (UEngine* engine = Globals::GEngine()())
				{
					engine->ForceGarbageCollection(true);
					Log::GetLog()->info("Empty-server maintenance: full garbage collection requested");
				}
			}
		}
	}

	void ApplyGameThreadSettings()
	{
		if (g_cfg.threadPriority > 0)
		{
			const int prio = g_cfg.threadPriority == 1 ? THREAD_PRIORITY_ABOVE_NORMAL
				: g_cfg.threadPriority == 2 ? THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_TIME_CRITICAL;
			const BOOL ok = SetThreadPriority(GetCurrentThread(), prio);
			Log::GetLog()->info("Game thread priority set to level {}: {}", g_cfg.threadPriority, ok ? "ok" : "refused");
		}
		if (g_cfg.threadAffinityMask != 0)
		{
			const DWORD_PTR prev = SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(g_cfg.threadAffinityMask));
			Log::GetLog()->info("Game thread affinity mask set to {}: {}", g_cfg.threadAffinityMask, prev != 0 ? "ok" : "refused");
		}
	}

	// restart inside a time window, preferring a moment when the server is empty
	std::string g_windowDone; // date of the window already handled

	bool ParseHHMM(const std::string& hhmm, int& h, int& m)
	{
		const auto colon = hhmm.find(':');
		if (colon == std::string::npos)
			return false;
		try
		{
			h = std::stoi(hhmm.substr(0, colon));
			m = std::stoi(hhmm.substr(colon + 1));
		}
		catch (...)
		{
			return false;
		}
		return h >= 0 && h <= 23 && m >= 0 && m <= 59;
	}

	void CheckRestartWindow()
	{
		if (!g_cfg.restartEnabled || !g_cfg.windowEnabled || g_restart.active)
			return;
		int sh, sm, eh, em;
		if (!ParseHHMM(g_cfg.windowStart, sh, sm) || !ParseHHMM(g_cfg.windowEnd, eh, em))
			return;

		const std::time_t now = std::time(nullptr);
		std::tm start = LocalTime(now);
		start.tm_hour = sh;
		start.tm_min = sm;
		start.tm_sec = 0;
		std::tm end = start;
		end.tm_hour = eh;
		end.tm_min = em;
		std::time_t ts = std::mktime(&start);
		std::time_t te = std::mktime(&end);
		if (te <= ts) // window crosses midnight
		{
			if (now < te)
				ts -= 24 * 3600;
			else
				te += 24 * 3600;
		}
		if (now < ts || now >= te)
			return;

		std::tm startTm = LocalTime(ts);
		char day[16];
		std::strftime(day, sizeof(day), "%Y-%m-%d", &startTm);
		if (g_windowDone == day)
			return;

		if (g_playersOnline == 0 && g_emptySeconds >= g_cfg.windowEmptyForMinutes * 60)
		{
			g_windowDone = day;
			StartCountdown(0, "restart window, server empty");
			return;
		}
		const int lead = g_cfg.warningMinutes.empty() ? 1 : g_cfg.warningMinutes.front();
		if (std::difftime(te, now) <= lead * 60.0)
		{
			g_windowDone = day;
			g_restart.active = true;
			g_restart.exitAt = te;
			g_restart.warned.clear();
			g_restart.saved = false;
			Log::GetLog()->info("Restart window ending with players online, counting down to {}", g_cfg.windowEnd);
			AnnounceCountdown();
		}
	}

	// ------------------------------------------------------------------------------------------------ timers

	int g_seconds = 0;

	void OnServerReady()
	{
		g_serverReady = true;
		g_serverReadyAt = Clock::now();
		const int n = ApplyCVars();
		ApplyGameThreadSettings();
		RunConsoleCommand("t.MaxFPS " + std::to_string(g_cfg.activeMaxFPS));
		Log::GetLog()->info("Server ready. Applied {} console variable(s). Wild cap {}, cleanup {}, restart schedule {}", n,
			g_cfg.capEnabled ? "on" : "off", g_cfg.cleanupEnabled ? "on" : "off", g_cfg.restartEnabled ? "on" : "off");
	}

	void OnSecond()
	{
		if (!g_serverReady)
		{
			if (AsaApi::GetApiUtils().GetStatus() == AsaApi::ServerStatus::Ready)
				OnServerReady();
			return;
		}
		++g_seconds;

		try
		{
			if (g_cfg.monitorEnabled && g_seconds % g_cfg.reportIntervalSeconds == 0)
			{
				g_lastReport = BuildReport(true);
				Log::GetLog()->info("{}", g_lastReport);
				if (g_tickHooksInstalled && g_cfg.bpEnabled)
					Log::GetLog()->info("{}", BlueprintSummary(true));
				if (g_tickHooksInstalled && g_cfg.profilerEnabled)
					Log::GetLog()->info("{}", ProfileSummary(true));
				if (g_cfg.mtEnabled)
					Log::GetLog()->info("{}", ThreadingSummary());
				if (g_cfg.throttleEnabled)
				{
					Log::GetLog()->info("{}", ThrottleSummary());
					g_throttleSkipped.store(0);
				}
				if (g_printBlocked > 0)
				{
					Log::GetLog()->info("Log filter blocked {} PrintString message(s)", g_printBlocked);
					g_printBlocked = 0;
				}
			}
			if (g_seconds % g_cfg.capCheckIntervalSeconds == 0)
				RunWildCap(false);
			if (g_cfg.cleanupEnabled && g_seconds % g_cfg.cleanupIntervalSeconds == 0)
				RunCleanup(false);
			if (g_cfg.cvarsReapplyMinutes > 0 && g_seconds % (g_cfg.cvarsReapplyMinutes * 60) == 0)
				ApplyCVars();
			if (g_seconds % g_cfg.throttleScanSeconds == 0)
				RebuildThrottle();
			UpdatePresence();
			if (g_seconds % g_cfg.mtScanSeconds == 0)
				UpdateAnyThread();
			CheckRestartWindow();
			CheckSchedule();
		}
		catch (const std::exception& e)
		{
			Log::GetLog()->error("Timer error: {}", e.what());
		}
	}

	// ------------------------------------------------------------------------------------------------ RCON

	std::string ArgsAfterCommand(RCONPacket* packet)
	{
		const std::string body = packet->Body.ToString();
		const auto space = body.find(' ');
		if (space == std::string::npos)
			return "";
		std::string rest = body.substr(space + 1);
		rest.erase(0, rest.find_first_not_of(" \t"));
		return rest;
	}

	void Reply(RCONClientConnection* conn, RCONPacket* packet, const std::string& text)
	{
		FString reply(text);
		conn->SendMessageW(packet->Id, 0, &reply);
	}

	void RconStats(RCONClientConnection* conn, RCONPacket* packet, UWorld*)
	{
		Reply(conn, packet, "Last report: " + g_lastReport + "\nNow: " + BuildReport(false));
	}

	void RconDinos(RCONClientConnection* conn, RCONPacket* packet, UWorld*)
	{
		const Census c = TakeCensus(World(), true);
		std::ostringstream s;
		const int cap = CurrentCap();
		s << "Wild living " << c.wildLiving << ", wild corpses " << c.wildDead << ", tamed " << c.tamedLiving << ", cap "
		  << (cap > 0 ? std::to_string(cap) : std::string("learning/off")) << ", baseline " << g_baseline
		  << (g_baselineLocked ? " (locked)" : "") << "\nTop species: " << TopSpecies(c, 12);
		Reply(conn, packet, s.str());
	}

	void RconCleanup(RCONClientConnection* conn, RCONPacket* packet, UWorld*)
	{
		Reply(conn, packet, RunWildCap(true) + "\n" + RunCleanup(true));
	}

	void RconRebaseline(RCONClientConnection* conn, RCONPacket* packet, UWorld*)
	{
		g_baseline = 0;
		g_baselineLocked = false;
		g_serverReadyAt = Clock::now();
		Reply(conn, packet, "Baseline reset. It will be re-learned between minute " +
			std::to_string(g_cfg.autoBaselineStartMinutes) + " and " + std::to_string(g_cfg.autoBaselineEndMinutes) + " from now.");
	}

	void RconExec(RCONClientConnection* conn, RCONPacket* packet, UWorld*)
	{
		const std::string cmd = ArgsAfterCommand(packet);
		if (cmd.empty())
		{
			Reply(conn, packet, "Usage: perf.exec <console command>, e.g. perf.exec a.ParallelAnimUpdate 1");
			return;
		}
		RunConsoleCommand(cmd);
		Log::GetLog()->info("perf.exec: {}", cmd);
		Reply(conn, packet, "Ran: " + cmd + " (the engine prints the result in the server log)");
	}

	void RconRestart(RCONClientConnection* conn, RCONPacket* packet, UWorld*)
	{
		const std::string arg = ArgsAfterCommand(packet);
		if (arg == "cancel")
		{
			g_restart.active = false;
			Broadcast(L"Server restart cancelled.");
			Reply(conn, packet, "Restart cancelled");
			return;
		}
		int minutes = 0;
		try { minutes = std::stoi(arg); }
		catch (...) { minutes = -1; }
		if (minutes < 0 || minutes > 240)
		{
			Reply(conn, packet, "Usage: perf.restart <minutes 0-240> | perf.restart cancel");
			return;
		}
		StartCountdown(minutes, "RCON");
		Reply(conn, packet, "Restart in " + std::to_string(minutes) + " minute(s)");
	}

	void RconReload(RCONClientConnection* conn, RCONPacket* packet, UWorld*)
	{
		const std::string msg = LoadConfig();
		g_anyThreadAborted = false;
		UpdateAnyThread(); // applies or restores immediately
		const int n = ApplyCVars();
		Reply(conn, packet, msg + ". Re-applied " + std::to_string(n) + " console variable(s).");
	}

	void RconProfile(RCONClientConnection* conn, RCONPacket* packet, UWorld*)
	{
		std::ostringstream s;
		s << ProfileSummary(false) << "\n" << ThrottleSummary() << "\n" << ThreadingSummary() << "\nPlayers online " << g_playersOnline << ", idle mode "
		  << (g_idleActive ? "on" : "off") << ", PrintString blocked " << g_printBlocked;
		Reply(conn, packet, s.str());
	}

	void RconBlueprints(RCONClientConnection* conn, RCONPacket* packet, UWorld*)
	{
		Reply(conn, packet, BlueprintSummary(false));
	}

	void RconHelp(RCONClientConnection* conn, RCONPacket* packet, UWorld*)
	{
		Reply(conn, packet,
			"PerfGuard commands: perf.stats | perf.dinos | perf.cleanup | perf.rebaseline | perf.exec <cmd> | "
			"perf.restart <minutes>|cancel | perf.reload | perf.profile | perf.bp");
	}

	const std::vector<std::pair<const char*, void (*)(RCONClientConnection*, RCONPacket*, UWorld*)>> kRcon = {
		{"perf.stats", &RconStats},       {"perf.dinos", &RconDinos},   {"perf.cleanup", &RconCleanup},
		{"perf.rebaseline", &RconRebaseline}, {"perf.exec", &RconExec}, {"perf.restart", &RconRestart},
		{"perf.reload", &RconReload},     {"perf.help", &RconHelp},       {"perf.profile", &RconProfile},
		{"perf.bp", &RconBlueprints},
	};
} // namespace PerfGuard

// ---------------------------------------------------------------------------------------------------- entry points

extern "C" __declspec(dllexport) void Plugin_Init()
{
	using namespace PerfGuard;
	Log::Get().Init(PROJECT_NAME);
	Log::GetLog()->info("{}", LoadConfig());
	InstallHooks();
	LoadTickLayout();

	AsaApi::GetCommands().AddOnTickCallback(FString("PerfGuard.Tick"), &OnTick);
	AsaApi::GetCommands().AddOnTimerCallback(FString("PerfGuard.Timer"), &OnSecond);
	for (const auto& [name, fn] : kRcon)
		AsaApi::GetCommands().AddRconCommand(FString(name), fn);

	if (AsaApi::GetApiUtils().GetStatus() == AsaApi::ServerStatus::Ready)
		OnServerReady();
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
	using namespace PerfGuard;
	{
		const bool keep = g_cfg.mtEnabled;
		g_cfg.mtEnabled = false;
		UpdateAnyThread(); // puts every flipped tick function back on the game thread
		g_cfg.mtEnabled = keep;
	}
	RemoveHooks();
	AsaApi::GetCommands().RemoveOnTickCallback(FString("PerfGuard.Tick"));
	AsaApi::GetCommands().RemoveOnTimerCallback(FString("PerfGuard.Timer"));
	for (const auto& [name, fn] : kRcon)
		AsaApi::GetCommands().RemoveRconCommand(FString(name));
}
