# PerfGuard

An [AsaApi](https://github.com/ArkServerApi/AsaApi) plugin for ARK: Survival Ascended servers. It does not change engine code. It reduces the work the server does and measures the result.

| Feature | What it does | Default |
|---|---|---|
| Tick monitor | Measures real frame times. Every 5 minutes it logs the average, p99 and worst frame, plus counts of players, wild dinos, corpses, tames and dropped items. | On |
| Wild overspawn cap | Between minute 10 and 30 after start, it learns the map's normal wild population. Later, it removes only the excess above that number plus 20%. It picks dinos farthest from players and never removes tames, bosses or dinos being tamed. | On |
| Stale cleanup | Removes wild corpses that have been dead 20+ minutes and dropped items older than 60 minutes. Both must be at least 150 m from every player. It never touches eggs, tamed corpses, player death caches or player-dropped items. | On |
| Scheduled restart | Warns players at 30/15/10/5/1 minutes, saves the world, then exits. Your panel's crash detection restarts the server. | Off |
| Engine console variables | Applies a list of `cvar value` commands at startup. Low-risk animation-threading cvars are on. Experimental ones are listed but off. | On |
| Profiler | Times dino tick, dino AI, movement, pathfinding, animation, status, player tick and game mode tick. Logs ms per frame every report and breaks down every hitch. | On |
| Distance throttle | Wild dinos 100–250 m from every player update every 3rd frame, beyond 250 m every 15th frame (actor tick, AI, pathfinding, movement and animation). Skipped time is passed on at the next update, so they don't fall behind. | Off |
| Idle mode | After 2 minutes with nobody online, caps the server at 10 FPS. Restores 30 FPS as soon as someone connects. | On |
| Empty-server maintenance | After 5 minutes empty, requests a full garbage collection (at most every 15 minutes), so the freeze never hits a player. | On |
| Restart window | Restarts the first time the server has been empty for 5 minutes between `WindowStart` and `WindowEnd`. If people play through the window, it counts down with warnings to `WindowEnd`. | Off |
| Game thread | Optional thread priority (`1` above normal, `2` highest, `3` time critical) and CPU affinity mask for the main game thread. | Off |

## Install

1. Download `PerfGuard.zip` from the latest successful **Actions** run (Artifacts) or from **Releases**.
2. Copy the `PerfGuard` folder into `ShooterGame/Binaries/Win64/ArkApi/Plugins/`. That folder holds `PerfGuard.dll`, `PluginInfo.json`, `config.json` and `config.performance.json`.
   - `config.json` is the cautious default (removals in dry-run, throttle off).
   - `config.performance.json` is the aggressive preset. To use it, rename it to `config.json`.
3. Restart the server. AsaApi logs go to `ShooterGame/Binaries/Win64/logs/ArkApi_<pid>.log`. Look for lines tagged `[PerfGuard]`.

## RCON / panel console commands

| Command | Result |
|---|---|
| `perf.stats` | Last 5-minute report plus a live measurement |
| `perf.dinos` | Wild/tamed counts, current cap, baseline, top 12 wild species |
| `perf.cleanup` | Run the wild cap and cleanup now and report what was removed |
| `perf.rebaseline` | Re-learn the wild baseline (run this after `destroywilddinos`) |
| `perf.exec <cmd>` | Run any console command, e.g. `perf.exec a.ParallelAnimUpdate 1` |
| `perf.restart <min>` / `perf.restart cancel` | Start or cancel a restart countdown |
| `perf.reload` | Reload `config.json` and re-apply cvars |
| `perf.profile` | Profiler breakdown, throttle counts, players online, idle state, blocked log messages |

## Measuring a change

1. Leave everything at defaults for a day and note the `Tick:` lines in the log.
2. Change one thing, either in `config.json` or with `perf.exec`.
3. Compare the next day's `Tick:` lines at a similar player count. **avg ms** is general load. **p99 / worst** are the hitches players feel.

## Experimental threading

Unreal can run a tick on a worker thread instead of the game thread when that tick's `bRunOnAnyThread` flag is set. ARK leaves it off for its own code. PerfGuard reads the flag's exact location from AsaApi's offsets cache, which AsaApi builds from the game's debug symbols, so it doesn't guess. It then sets the flag on the wild-dino ticks you choose:

| Setting | Ticks moved to worker threads |
|---|---|
| `DinoActorTick` | The dino's own `Tick` (stats, timers, Blueprint tick) |
| `DinoAIControllerTick` | The dino's AI controller |
| `DinoMovement` | Character movement (most likely to crash: touches physics and collision) |
| `DinoMesh` | Skeletal mesh / animation |
| `IncludeTamed` | Also tamed dinos (off: crashes would hit your own dinos) |
| `OnlyBeyondDistance` | Only dinos at least this far from every player (Unreal units, 10000 = 100 m) |

**Safety checks:**
- It checks every selected tick function reports `bCanEverTick`. If the layout looks wrong, it aborts and puts everything back.
- It only touches dinos it found in the current scan, never freed memory.
- Switching it off, `perf.reload` or unloading the plugin moves every tick back to the game thread.
- `perf.profile` shows how many ticks are on worker threads.

**Expect crashes.** ARK's code assumes one thread. Ticks running at the same time as the game thread can corrupt data or crash, and a crash may only appear after minutes or hours. Test it on a throwaway save. The zip includes `config.threading.json`, which is the performance preset plus threading for the actor and AI ticks of wild dinos beyond 100 m. Rename it to `config.json` to try it.

## What the 70,000-dino log showed (v2.4)

- **Stasis:** ARK already puts dinos far from players into stasis or hibernation. Only a handful of dino updates run per frame, so distance throttle and experimental threading have little to act on. Both are off in every preset now. They're still available for testing.
- **Scan cost:** in 2.3 and earlier, each scan of every dino cost most of a second under FEX, because of per-field name lookups. 2.4 resolves field locations once, checks them against AsaApi's accessors, then reads memory directly. Any scan over 50 ms is logged with its time.
- **Profiler:** it now also times wild spawn zones (`UpdateZone`, `CheckIncreaseNPC`, `SpawnNPCs`) and hibernation wake-up (`AwakenActorsForPlayers`). Those are where a huge wild population costs time while players move around.

## Config notes

- **Log filter removed:** v2.0 hooked Blueprint `PrintString` to drop "CS Cast Failed" spam, but that function's layout differs on ARK 97.21 and it crashed the server. To hide the spam, filter the console in the startup command instead: `tail ... | grep --line-buffered -v "CS Cast Failed"`.
- **Restart needed for hooks:** turning `Profiler` or `DistanceThrottle` on or off in the config needs a server restart. The hooks are installed at load. Their settings, like distances and frame counts, apply on `perf.reload`.

- **Units:** distances are Unreal units, where 100 = 1 m and one foundation = 300.
- **`WildDinoCap.MaxWildDinos`:** set a fixed cap instead of the learned baseline (0 = automatic).
- **`WildDinoCap.PerSpeciesMax`:** caps any single species. Use it when `perf.dinos` shows one species flooding the map.
- **`WildDinoCap.ExcludeSpecies`:** names (partial match) that are never removed, e.g. `["Wyvern", "Rock_Drake"]`.
- **`DryRun`:** set it in either section to log what would be removed without removing anything.
- **`ScheduledRestart.Times`:** uses the server's local clock (usually UTC inside a container). The panel must restart the server after it exits, so check that crash detection or auto-restart is on.
- **CVars and PerformaceGC:** the experimental GC cvars conflict with PerformaceGC's GC settings. Don't enable both.

## Building

GitHub Actions builds the plugin on every push to `main` (`.github/workflows/build.yml`):

- The headers come from the AsaApi tag in `ASAAPI_VERSION`.
- `AsaApi.lib` comes from that AsaApi GitHub release. If the release has no zip, copy `Lib/AsaApi.lib` from the AsaApi download you installed on the server to `lib/AsaApi.lib` in this repo.
- Every push to `main` updates the `latest` release with a ready-to-install `PerfGuard.zip`.

To build locally, use Visual Studio 2022 with the C++ workload. Run `git clone --branch 2.11 https://github.com/ArkServerApi/AsaApi extern/AsaApi`, put `AsaApi.lib` in `extern/AsaApi/out_lib/`, then build `PerfGuard.sln` (Release x64).

## Limits

- It can't make FEX emulation or ARK's single game thread faster. It only removes avoidable work.
- It hasn't been tested on a live server yet. Both removal features ship with `DryRun: true`, so they only log what they would remove. Check `perf.cleanup` and the log, then set `DryRun` to `false` and run `perf.reload`.
- AsaApi plugins must match the AsaApi version on the server. Rebuild after AsaApi updates.
