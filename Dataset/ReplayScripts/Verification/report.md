# Replay verification: dune-sentinel at 33df55d067

## Verdict: PASS

| criterion | result | pass |
|---|---|---|
| every tracked file outside Content/ byte-identical; none missing; no extra files except git-ignored ones | 499 identical, 0 differ, 0 missing; 0 extra (0 git-ignored) | yes |
| every asset of the final commit present with the same class and structure digest | 528 same, 0 differ, 0 missing, 0 extra, 0 class changed | yes |
| every game map: same actors (class, label, folder) with the same transform, components, properties and world settings | DesertDefense 1193/1193 | yes |

Known differences that are not content (normalised or not compared):

- content_bytes: Content/ .uasset/.umap bytes: Unreal writes a random PersistentGuid into every saved package; compared by structure instead (criteria assets, maps) (0)
- maps_not_compared: outside the game's map roots ['/Game/Maps']: demo/showcase levels shipped inside store packs, not built by the game's creator (0 maps: )
- git_ignored_extra_files: 0 file(s)
- guid: random GUIDs Unreal writes on save (ExpressionGUID, Guid=...) (0)
- loc_key: random localization keys Unreal generates for text properties (NSLOCTEXT("", "<32 hex>", ...)) (0)
- memory_address: memory addresses in printed object values (delegates: '(0x00000801...)') (0)
- project_path: paths into the folder the editor ran in (game repo vs replay project, user profile name; absolute or project-relative) in import-source fields (0)
- derived_compression_size: AnimSequence compressed size: derived data of the compression codec run, not authored content (key tags.Compressed Size (KB)) (0)

## Deviations from the recording

Everything below is done from the game's replay settings (`game.json`) rather than straight from the recorded
actions; each entry says why.

### shell_steps (18)

shell commands of the builder replayed from settings (delete, move-merge, host-python), each matched to its recorded command.

- match: reuse_native_state_v2.py' --state s01; argv: keep-committed, Content; reason: Remove content absent from this operation's adopted state.
- match: reuse_native_state_v2.py' --state s02; argv: keep-committed, Content; reason: Remove content absent from this operation's adopted state.
- match: reuse_native_state_v2.py' --state s03; argv: keep-committed, Content; reason: Remove content absent from this operation's adopted state.
- match: reuse_native_state_v2.py' --state s04; argv: keep-committed, Content; reason: Remove content absent from this operation's adopted state.
- match: reuse_native_state_v2.py' --state s05; argv: keep-committed, Content; reason: Remove content absent from this operation's adopted state.
- match: reuse_native_state_v2.py' --state s06; argv: keep-committed, Content; reason: Remove content absent from this operation's adopted state.
- match: reuse_native_state_v2.py' --state s07; argv: keep-committed, Content; reason: Remove content absent from this operation's adopted state.
- match: reuse_native_state_v2.py' --state s08; argv: keep-committed, Content; reason: Remove content absent from this operation's adopted state.
- match: reuse_native_state_v2.py' --state s09; argv: keep-committed, Content; reason: Remove content absent from this operation's adopted state.
- match: reuse_native_state_v2.py' --state s10; argv: keep-committed, Content; reason: Remove content absent from this operation's adopted state.
- match: reuse_native_state_v2.py' --state s11; argv: keep-committed, Content; reason: Remove content absent from this operation's adopted state.
- match: reuse_native_state_v2.py' --state s12; argv: keep-committed, Content; reason: Remove content absent from this operation's adopted state.
- match: reuse_native_state_v2.py' --state s13; argv: keep-committed, Content; reason: Remove content absent from this operation's adopted state.
- match: reuse_native_state_v2.py' --state s14; argv: keep-committed, Content; reason: Remove content absent from this operation's adopted state.
- match: reuse_native_state_v2.py' --state s15; argv: keep-committed, Content; reason: Remove content absent from this operation's adopted state.
- match: reuse_native_state_v2.py' --state s16; argv: keep-committed, Content; reason: Remove content absent from this operation's adopted state.
- match: reuse_native_state_v2.py' --state s17; argv: keep-committed, Content; reason: Remove content absent from this operation's adopted state.
- match: reuse_native_state_v2.py' --state final; argv: keep-committed, Content; reason: Remove content absent from this operation's adopted state.

### asset_drops (18)

store packs and downloads restored into the project where the builder copied them in.

- id: reuse-s01; packs: None; reason: Restore the native bytes adopted in this recorded reuse operation.
- id: reuse-s02; packs: None; reason: Restore the native bytes adopted in this recorded reuse operation.
- id: reuse-s03; packs: None; reason: Restore the native bytes adopted in this recorded reuse operation.
- id: reuse-s04; packs: None; reason: Restore the native bytes adopted in this recorded reuse operation.
- id: reuse-s05; packs: None; reason: Restore the native bytes adopted in this recorded reuse operation.
- id: reuse-s06; packs: None; reason: Restore the native bytes adopted in this recorded reuse operation.
- id: reuse-s07; packs: None; reason: Restore the native bytes adopted in this recorded reuse operation.
- id: reuse-s08; packs: None; reason: Restore the native bytes adopted in this recorded reuse operation.
- id: reuse-s09; packs: None; reason: Restore the native bytes adopted in this recorded reuse operation.
- id: reuse-s10; packs: None; reason: Restore the native bytes adopted in this recorded reuse operation.
- id: reuse-s11; packs: None; reason: Restore the native bytes adopted in this recorded reuse operation.
- id: reuse-s12; packs: None; reason: Restore the native bytes adopted in this recorded reuse operation.
- id: reuse-s13; packs: None; reason: Restore the native bytes adopted in this recorded reuse operation.
- id: reuse-s14; packs: None; reason: Restore the native bytes adopted in this recorded reuse operation.
- id: reuse-s15; packs: None; reason: Restore the native bytes adopted in this recorded reuse operation.
- id: reuse-s16; packs: None; reason: Restore the native bytes adopted in this recorded reuse operation.
- id: reuse-s17; packs: None; reason: Restore the native bytes adopted in this recorded reuse operation.
- id: reuse-final; packs: None; reason: Restore the native bytes adopted in this recorded reuse operation.

## Files against the final commit

| | identical | differs | missing |
|---|---:|---:|---:|
| outside Content/ | 499 | 0 | 0 |
| Content/ | 527 | 0 | 0 |

Files in the replay project that the commit does not have: 0.

## Assets (structure dump)

Same class and digest: 528; differs: 0; missing: 0; extra: 0.

| class | same | differs | missing | extra |
|---|---:|---:|---:|---:|
| Blueprint | 1 | 0 | 0 | 0 |
| BlueprintGeneratedClass | 1 | 0 | 0 | 0 |
| Font | 2 | 0 | 0 | 0 |
| FontFace | 2 | 0 | 0 | 0 |
| LandscapeLayerInfoObject | 4 | 0 | 0 | 0 |
| Material | 127 | 0 | 0 | 0 |
| MaterialFunction | 2 | 0 | 0 | 0 |
| MaterialInstanceConstant | 33 | 0 | 0 | 0 |
| SoundWave | 12 | 0 | 0 | 0 |
| StaticMesh | 55 | 0 | 0 | 0 |
| Texture2D | 288 | 0 | 0 | 0 |
| World | 1 | 0 | 0 | 0 |

## Maps (actor structure)

| map | actors (truth / replay) | matched | identical | differs | missing | extra | fields that differ |
|---|---|---:|---:|---:|---:|---:|---|
| DesertDefense | 1193 / 1193 | 1193 | 1193 | 0 | 0 | 0 |  |

## Script runs: workflow rows replayed vs original



## Windows

| # | commit | runs | seconds | fallback | exact | differs | missing | extra |
|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 1 | a74f513 | 0 | 4 | 0 | 50 | 0 | 0 | 0 |
| 2 | e32a197 | 0 | 6 | 0 | 5 | 0 | 0 | 0 |
| 3 | fd085ea | 0 | 4 | 0 | 0 | 0 | 0 | 0 |
| 4 | 5a94224 | 0 | 4 | 0 | 0 | 0 | 0 | 0 |
| 5 | 934279b | 0 | 4 | 0 | 4 | 0 | 0 | 0 |
| 6 | 6d949ae | 0 | 4 | 0 | 0 | 0 | 0 | 0 |
| 7 | 460deeb | 0 | 4 | 0 | 0 | 0 | 0 | 0 |
| 8 | 482feef | 0 | 7 | 0 | 44 | 0 | 0 | 0 |
| 9 | c4f5299 | 0 | 6 | 0 | 7 | 0 | 0 | 0 |
| 10 | 6431c4f | 0 | 4 | 0 | 0 | 0 | 0 | 0 |
| 11 | c541a1f | 0 | 12 | 0 | 125 | 0 | 0 | 0 |
| 12 | dc97d88 | 0 | 7 | 0 | 0 | 0 | 0 | 0 |
| 13 | 28cd1fd | 0 | 7 | 0 | 2 | 0 | 0 | 0 |
| 14 | 52d1ad6 | 0 | 13 | 0 | 232 | 0 | 0 | 0 |
| 15 | d582dfc | 0 | 9 | 0 | 0 | 0 | 0 | 0 |
| 16 | 03bbda4 | 0 | 41 | 0 | 620 | 0 | 0 | 0 |
| 17 | 061c686 | 0 | 32 | 0 | 9 | 0 | 0 | 0 |
| 18 | 32d1e06 | 0 | 16 | 0 | 360 | 0 | 0 | 0 |
| 19 | 33df55d | 0 | 3 | 0 | 0 | 0 | 0 | 0 |
