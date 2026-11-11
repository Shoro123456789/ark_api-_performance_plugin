# PerfGuard

An [AsaApi](https://github.com/ArkServerApi/AsaApi) plugin for ARK: Survival Ascended servers. It does not change engine code. It reduces the work the server does and measures the result.

| Feature | What it does | Default |
|---|---|---|
| Tick monitor | Measures real frame times. Every 5 minutes it logs the average, p99 and worst frame, plus counts of players, wild dinos, corpses, tames and dropped items. | On |
| Wild overspawn cap | Between minute 10 and 30 after start, it learns the map's normal wild population. Later, it removes only the excess above that number plus 20%. It picks dinos farthest from players and never removes tames, bosses or dinos being tamed. | On |
| Stale cleanup | Removes wild corpses that have been dead 20+ minutes and dropped items older than 60 minutes. Both must be at least 150 m from every player. It never touches eggs, tamed corpses, player death caches or player-dropped items. | On |
| Scheduled restart | Warns players at 30/15/10/5/1 minutes, saves the world, then exits. Your panel's crash detection restarts the server. | Off |
| Engine console variables | Applies a list of `cvar value` commands at startup. Low-risk animation-threading cvars are on. Experimental ones are listed but off. | On |

## Install

1. Download `PerfGuard.zip` from the latest successful **Actions** run (Artifacts) or from **Releases**.
2. Copy the `PerfGuard` folder into `ShooterGame/Binaries/Win64/ArkApi/Plugins/`. That folder holds `PerfGuard.dll`, `PluginInfo.json` and `config.json`.
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

## Measuring a change

1. Leave everything at defaults for a day and note the `Tick:` lines in the log.
2. Change one thing, either in `config.json` or with `perf.exec`.
3. Compare the next day's `Tick:` lines at a similar player count. **avg ms** is general load. **p99 / worst** are the hitches players feel.

## Config notes

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
- Push a tag like `v1.0.0` to also publish a release.

To build locally, use Visual Studio 2022 with the C++ workload. Run `git clone --branch 2.11 https://github.com/ArkServerApi/AsaApi extern/AsaApi`, put `AsaApi.lib` in `extern/AsaApi/out_lib/`, then build `PerfGuard.sln` (Release x64).

## Limits

- It can't make FEX emulation or ARK's single game thread faster. It only removes avoidable work.
- It hasn't been tested on a live server yet. Both removal features ship with `DryRun: true`, so they only log what they would remove. Check `perf.cleanup` and the log, then set `DryRun` to `false` and run `perf.reload`.
- AsaApi plugins must match the AsaApi version on the server. Rebuild after AsaApi updates.
