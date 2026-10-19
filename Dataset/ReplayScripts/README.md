# Replay the recorded 5.7 asset adoption

This replay reproduces eighteen recorded adoption operations and their content
pruning from the new history's own commits and LFS objects. It does not claim
that the reused native assets were authored again. Verification/report.json is
the retained result of the completed release replay, not a result of a new run
on your computer.

Use the supplied ledger.json. Do not run the ledger, live or release commands:
the original action-recording directory is not an input to this delivery.
The portable configuration has recording disabled. Its explicit engine path
describes the validation machine; set it to your installed Unreal Engine 5.7.
Do not replace it with an engine placeholder that could select another version.

Requirements: Windows, Unreal Engine 5.7 and its C++ toolchain, Python 3.11+,
Git LFS, and the configured machine limiter at GF_ROOT/tools/ue-heavy.ps1.
Run the following from this ReplayScripts directory, using fresh destinations.
The repository mirror omits History; use History from the full archive.

```powershell
$ErrorActionPreference = 'Stop'
$PreviousSkipSmudge = [Environment]::GetEnvironmentVariable('GIT_LFS_SKIP_SMUDGE', 'Process')
Get-ChildItem Env:GIT_* | ForEach-Object { Remove-Item -LiteralPath ('Env:' + $_.Name) }
$Replay = (Get-Location).Path
$Restored = 'E:/RestoredGames/dune-sentinel'
$ReplayWork = 'E:/ReplayWork/dune-sentinel-fresh'
$Engine57 = 'E:/EpicGames/UE_5.7'
$env:GF_ROOT = 'E:/ConfiguredWorkRoot'
$env:TEMP = 'E:/ReplayTemp'
$env:TMP = $env:TEMP
if ((Test-Path -LiteralPath $Restored) -or (Test-Path -LiteralPath $ReplayWork)) { throw 'Choose fresh restore and replay directories' }
if (-not (Test-Path -LiteralPath "$env:GF_ROOT/tools/ue-heavy.ps1")) { throw 'Configure the machine limiter first' }
if (-not (Test-Path -LiteralPath $env:TEMP)) { throw 'Create the explicit temporary directory first' }
$Config = Get-Content -Raw -LiteralPath "$Replay/game.json" | ConvertFrom-Json
try {
  $env:GIT_LFS_SKIP_SMUDGE = '1'
  git clone --no-checkout ../History/dune-sentinel.bundle $Restored
  if ($LASTEXITCODE -ne 0) { throw 'History restoration failed' }
} finally {
  [Environment]::SetEnvironmentVariable('GIT_LFS_SKIP_SMUDGE', $PreviousSkipSmudge, 'Process')
}
New-Item -ItemType Directory -Path "$Restored/.git/lfs/objects" -Force | Out-Null
Copy-Item -Path '../History/lfs/objects/*' -Destination "$Restored/.git/lfs/objects" -Recurse
git -C $Restored lfs install --local
if ($LASTEXITCODE -ne 0) { throw 'Local LFS setup failed' }
git -C $Restored checkout --detach $Config.final
if ($LASTEXITCODE -ne 0) { throw 'Release checkout failed' }
git -C $Restored lfs checkout
if ($LASTEXITCODE -ne 0) { throw 'LFS checkout failed' }
$Config.repo = $Restored
$Config.engine = $Engine57
$Config.work_root = $ReplayWork
$Config.recording = ''
$Config.live = $false
$LocalConfig = Join-Path $Replay 'local.game.json'
if (Test-Path -LiteralPath $LocalConfig) { throw 'Preserve the previous local configuration' }
[IO.File]::WriteAllText($LocalConfig, ($Config | ConvertTo-Json -Depth 100), (New-Object Text.UTF8Encoding($false)))
New-Item -ItemType Directory -Path "$ReplayWork/work" | Out-Null
Copy-Item -LiteralPath "$Replay/ledger.json" -Destination "$ReplayWork/work/ledger.json"
if (Test-Path -LiteralPath "$Replay/Blobs") { Copy-Item -LiteralPath "$Replay/Blobs" -Destination "$ReplayWork/work/blobs" -Recurse }
python -B ue_replay.py prepare --game local.game.json
if ($LASTEXITCODE -ne 0) { throw 'Replay preparation failed' }
python -B ue_replay.py run --game local.game.json
if ($LASTEXITCODE -ne 0) { throw 'Replay execution failed' }
python -B ue_replay.py truth --game local.game.json
if ($LASTEXITCODE -ne 0) { throw 'Truth build failed' }
python -B -c "import ue_replay,runner; c=ue_replay.load_cfg('local.game.json'); x=runner.Ctx(c); x.set_engine(x.engine_for(x.uproject)); r=runner.build_editor(x,'replay_release',sha=c['final']); print(r); raise SystemExit(r[0])"
if ($LASTEXITCODE -ne 0) { throw 'Replay editor build failed' }
python -B ue_replay.py dump --game local.game.json --which replay
if ($LASTEXITCODE -ne 0) { throw 'Replay structure dump failed' }
$ReplayDone = Get-Content -Raw -LiteralPath "$ReplayWork/work/dump-replay/done.json" | ConvertFrom-Json
if (-not $ReplayDone.engine.StartsWith('5.7.')) { throw 'The fresh replay dump did not finish on UE 5.7' }
python -B ue_replay.py dump --game local.game.json --which truth
if ($LASTEXITCODE -ne 0) { throw 'Truth structure dump failed' }
$TruthDone = Get-Content -Raw -LiteralPath "$ReplayWork/work/dump-truth/done.json" | ConvertFrom-Json
if (-not $TruthDone.engine.StartsWith('5.7.')) { throw 'The fresh truth dump did not finish on UE 5.7' }
python -B ue_replay.py verify --game local.game.json
if ($LASTEXITCODE -ne 0) { throw 'New replay verification failed' }
```

Inspect the new work/Verification/report.json and report.md under your fresh
replay directory. The retained delivery report remains unchanged. These commands
use the official runner and machine limiter, and produce no screenshots or video.
