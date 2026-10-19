# Dune Sentinel

A Windows air-combat game for Unreal Engine 5.7. Defend a desert airbase, choose a fighter and intercept hostile aircraft while managing ammunition and speed. The scene includes a detailed airport, nearby settlements, rugged desert terrain and oases.

## Development

Open `DuneSentinel.uproject` with Unreal Engine 5.7 and build `DuneSentinelEditor`. Fetch Git LFS objects before opening a fresh clone. The development record includes the actual adoption of completed project components and subsequent changes. Existing native assets are reused directly; this does not claim that they were imported again.

## Controls

Enter launches from the main menu. W pitches up and S pitches down; A/D roll and Q/E yaw. Hold Space to accelerate and Ctrl to brake. Left mouse fires the cannon; F fires a missile. Tab cycles targets and holding right mouse looks at the selected enemy; release it to return to the normal flight view. Mouse flight, Alt free look, V camera switching and Esc pause retain their existing behavior.

## Assets

See `assets.manifest.json` and `Docs/ThirdPartyNotices.md` for asset sources and licences.

## Tasks

Evaluation tasks are maintained on the separate `tasks` branch. Each task identifies a starting revision and a later completed revision on `main`.

## Verification

The retained release contains 527 native resource packages. Each adopted state has an exact-byte reuse receipt under `Docs/validation/reuse/`. `ReplayConfig.json` restores the inputs from their recorded commits and removes assets absent from each state. This reproduces the recorded adoption and changes; it does not claim that adopted artwork was imported again.

Gameplay evidence, replay verification and packaged-build checks are reported separately in the delivery metadata. Existing gameplay media are reference material. Current checks produce no new screenshots or video.

## Delivery

The validated dataset mirror is in `Dataset/`. The complete archive is `Dataset.zip`, kept outside Git; it also includes the project, Windows Shipping build, anonymized history with LFS objects and separated evaluation documents. The archive records the release source revision.

The current keyboard layout update is verified in [its focused acceptance report](Docs/validation/keyboard-flight-layout.md). The retained Dataset archive and earlier Windows Shipping executable predate this update and still use the earlier controls; this source update does not regenerate that release package.

Use [the portable replay instructions](Dataset/ReplayScripts/README.md) with the History folder from the complete archive. The repository-level replay configuration records the local authoring context; the portable instructions describe restoring the delivered history and configuring a fresh machine.

[Release verification](Docs/validation/release.json) distinguishes the packaged smoke test, prior task-route evidence verified through identical runtime inputs, and the complete recorded asset-adoption replay.

## Reference gallery

The following retained gameplay stills are reference material, not new captures of this release.

![Retained gameplay reference](Dataset/Screenshots/dune-sentinel_S1_20261016-025526_s42_02.jpg)

![Retained gameplay reference 2](Dataset/Screenshots/dune-sentinel_S1_20261016-025526_s42_03.jpg)

![Retained gameplay reference 3](Dataset/Screenshots/dune-sentinel_S1_20261016-025526_s42_04.jpg)

![Retained gameplay reference 4](Dataset/Screenshots/dune-sentinel_S1_20261016-025526_s42_05.jpg)

![Retained gameplay reference 5](Dataset/Screenshots/dune-sentinel_S1_20261016-025526_s42_06.jpg)

![Retained gameplay reference 6](Dataset/Screenshots/dune-sentinel_S1_20261016-025526_s42_07.jpg)

![Retained gameplay reference 7](Dataset/Screenshots/dune-sentinel_S1_20261016-025526_s42_08.jpg)
