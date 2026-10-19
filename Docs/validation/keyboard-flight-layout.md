# Keyboard flight layout acceptance

Result: the requested controls passed a focused runtime check on Unreal Engine 5.7. The tested revision is `139b173ba7ac26a8f4e3b04e1ea214480520f0c4`; its gameplay change is `1a3798b006d7b5c4ef862578d737dd805eb92f23`, following the old-layout seed `cc6bc95f0646d221b15a9ca0ef12b276a9045ae2`.

## Scope and method

One incremental `DuneSentinelEditor Win64 Development` build passed (47.5 seconds of compilation). One rendered game run lasted 60.005 game seconds and exited with code 0. It opened `/Game/Maps/DesertDefense`, pressed Enter, and exercised the new controls in the first 27 seconds, followed by an idle observation period. All 20 scheduled press/release events were dispatched and retained. No game state was overridden. No new video or screenshot was saved.

The route is [Tests/keyboard_flight_layout_route.json](../../Tests/keyboard_flight_layout_route.json). The external observer applies ordinary controller key events through the active input mappings and records numeric state; it does not call the game's action handlers directly. Its unchanged source is retained with the independent validation material. The observer was used only in the private build clone and removed from that clone after the run, with its files preserved outside the project.

## Observed results

| Control | Observed result |
| --- | --- |
| W: pitch up | Pitch rose from 4.00 degrees to 31.20 degrees after the short hold. |
| S: pitch down | Pitch returned from 31.20 degrees to 4.05 degrees after the short hold. |
| Space: accelerate | Speed rose from 232.98 to 364.16 m/s; missile ammunition stayed at 16. |
| Left Ctrl: brake | Speed fell from 341.17 to 240.83 m/s; cannon ammunition stayed at 1,200. |
| Left mouse: cannon | Cannon ammunition fell from 1,200 to 1,183 during the one-second hold, then stayed at 1,183 after release. |
| F: missile | Missile ammunition fell from 16 to 15; cannon ammunition and the ordinary camera view stayed unchanged. |
| Right mouse: target view | After Tab selected an enemy, camera yaw moved from 0.00 to 147.94 degrees while held, then returned to 0.24 degrees after release. Missile ammunition stayed at 15. |
| Right Ctrl: brake | Speed fell from 237.55 to 190.94 m/s; cannon ammunition stayed at 1,183. |

Flight input settled after release, with pitch stable at 4.05 degrees through the remaining checks. Both weapons remained separate from the throttle, brake and view controls. The in-game help and README were checked against the new bindings. Other flight handling, weapons, assets and controls were not changed.

## Evidence and limitations

[Build receipt](keyboard-flight-layout/build.json), [run summary](keyboard-flight-layout/summary.json), [input events](keyboard-flight-layout/events.jsonl), [compact state trace](keyboard-flight-layout/trace.jsonl), and [raw evidence hashes](keyboard-flight-layout/raw-evidence-hashes.json) preserve the results. The compact trace selects relevant fields from the raw telemetry without changing their values. Raw output remains under `Saved/Validation/KeyboardLayout/r1/` locally.

The run produced 6,127 rendered frames and 119 nonblank numeric framebuffer observations. These establish rendered operation, not an artistic-quality assessment. One external-observer append failed: 289 of 290 attempted state samples were retained, with a maximum gap of 0.407 seconds. All input events and the before/during/after samples needed for the control checks are present, so another gameplay run was not needed. There was no engine crash or assertion. During the subsequent hands-off observation, the aircraft was lost at 43.112 seconds; all control checks had already finished by 27 seconds. This is recorded in the trace and is not counted as a control failure or a completed combat scenario.

The official repository/task format check (`validate-game.py --no-zip`) returned **VALID, 0 errors and 0 warnings**, including all 11 tasks and the retained repository Dataset mirror. See [the format result](keyboard-flight-layout/repository-format.json).

Verification was limited to this build, the single control route, a review of the narrow source changes and that format check. Earlier historical task routes were not rerun. The complete Dataset archive and previous Shipping executable were not rebuilt and retain their previous controls. No upload was performed.
