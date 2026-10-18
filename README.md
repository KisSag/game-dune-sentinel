# Dune Sentinel

A Windows air-combat game for Unreal Engine 5.7. Defend a desert airbase, choose a fighter and intercept hostile aircraft while managing ammunition and speed. The scene includes a detailed airport, nearby settlements, rugged desert terrain and oases.

## Development

Open `DuneSentinel.uproject` with Unreal Engine 5.7 and build `DuneSentinelEditor`. Fetch Git LFS objects before opening a fresh clone. The development record includes the actual adoption of completed project components and subsequent changes. Existing native assets are reused directly; this does not claim that they were imported again.

## Controls

Enter launches from the main menu. W increases throttle; S brakes. A/D roll, Up/Down pitch, Q/E yaw. Left mouse or Left Ctrl fires the cannon; right mouse or Space fires missiles. Tab cycles targets and F looks at the target. Esc pauses.

## Assets

See `assets.manifest.json` and `Docs/ThirdPartyNotices.md` for asset sources and licences.

## Tasks

Evaluation tasks are maintained on the separate `tasks` branch. Each task identifies a starting revision and a later completed revision on `main`.

## Verification

The retained release contains 527 native resource packages. Each adopted state has an exact-byte reuse receipt under `Docs/validation/reuse/`. `ReplayConfig.json` restores the inputs from their recorded commits and removes assets absent from each state. This reproduces the recorded adoption and changes; it does not claim that adopted artwork was imported again.

Gameplay evidence, replay verification and packaged-build checks are reported separately in the delivery metadata. Existing gameplay media are reference material. Current checks produce no new screenshots or video.
