<#
.SYNOPSIS
    Lift Nightfire's two XBEs into src/recomp/<target>/gen with the xboxrecomp tools.

.DESCRIPTION
    The disc has two executables with separate engines:
      main     game/default.xbe  -> src/recomp/main/gen     (nightfire.exe)
      driving  game/Driving.xbe  -> src/recomp/driving/gen  (nightfire_driving.exe)

    Each runs parse -> disasm -> identify -> abi -> lift, writing every stage's
    output under build/pipeline/<target>, so nothing lands beside the game
    files. The seeds come from config/seeds/<target>.json; the functions that
    src/manual_<target>.c defines by hand are left out of the generated code.
    With -Target all the two targets run side by side.

    Seeds are applied by the disasm stage: after editing a seed file, run with
    -From disasm (not -From lift), or the new seeds are ignored.

    Re-runs are cheap: disasm keeps its own cache, identify/abi/lift are
    skipped while nothing they read has changed, and only generated files
    whose content changed are rewritten, so the build recompiles just those.

.PARAMETER Target
    main, driving or all. Default: all.

.PARAMETER From
    Resume at a stage: parse, disasm, identify, abi or lift. Default: parse.
    identify, abi and lift always run when named here.

.PARAMETER NoPrepare
    With -Target all, scripts/build.ps1 -Prepare runs beside the two lifts
    and compiles the toolkit and the launcher on the cores they leave free
    (log: build/prepare.log). This switch leaves that out.

.EXAMPLE
    ./scripts/recompile.ps1
    ./scripts/recompile.ps1 -Target driving -From disasm
#>
param(
    [ValidateSet('main', 'driving', 'all')]
    [string]$Target = 'all',
    [ValidateSet('parse', 'disasm', 'identify', 'abi', 'lift')]
    [string]$From = 'parse',
    [switch]$NoPrepare
)

$ErrorActionPreference = 'Stop'
# A click into the window would otherwise pause the lift until Enter.
. (Join-Path $PSScriptRoot 'quickedit.ps1')
$root = Split-Path -Parent $PSScriptRoot
$toolkit = Join-Path $root 'external/xboxrecomp'

if (-not (Test-Path (Join-Path $toolkit 'tools/recomp'))) {
    throw "xboxrecomp not found at $toolkit. Run: git submodule update --init --recursive"
}

# The Python the tools run with: toolchain\python, the embedded Python with
# capstone that a player's install carries (see installer\), when it is there;
# else the py launcher's Python 3.
$bundledPy = Join-Path $root 'toolchain\python\python.exe'
if (Test-Path $bundledPy) {
    $pyExe = $bundledPy
} else {
    # The py launcher asked once for the python.exe it means, and that one
    # called from then on. The Python install manager's py.exe has, while
    # updating itself, dropped its arguments and opened the interactive
    # prompt -- with the output in a log that prompt failed on every line and
    # wrote gigabytes. A python.exe called directly has no such step.
    $ErrorActionPreference = 'Continue'
    $pyExe = ('' | & py -3 -c 'import sys; print(sys.executable)' 2>$null | Select-Object -Last 1)
    $ErrorActionPreference = 'Stop'
    if (-not $pyExe -or -not (Test-Path -LiteralPath $pyExe.Trim())) {
        throw "Python 3 not found: 'py -3' gave no python.exe. Install Python 3.10+ with capstone, or use Nightfire-Setup.exe, which brings its own."
    }
    $pyExe = $pyExe.Trim()
}
$pyPre = @()
# disasm's asm/*.asm listings are for reading (tens of MB); a player's install
# has no use for them.
$disasmExtra = if (Test-Path $bundledPy) { @('--no-listings') } else { @() }

# Both targets at once, each in its own PowerShell sharing this console. The
# two pipelines share nothing, and main's disasm alone outlasts all of
# driving's stages.
if ($Target -eq 'all') {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $procs = foreach ($n in @('main', 'driving')) {
        $p = Start-Process powershell -NoNewWindow -PassThru -ArgumentList @(
            '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"",
            '-Target', $n, '-From', $From)
        $null = $p.Handle   # without this, ExitCode reads back empty in PS 5.1
        @{ Name = $n; Proc = $p }
    }
    # The toolkit and the launcher need no generated code: compiled now, at
    # below-normal priority, on the cores the lifts leave free (disasm uses
    # one each), the build afterwards only has the game left to do.
    $prep = $null
    if (-not $NoPrepare) {
        $prepLog = Join-Path $root 'build\prepare.log'
        New-Item -ItemType Directory -Force (Split-Path $prepLog) | Out-Null
        $prep = Start-Process powershell -NoNewWindow -PassThru `
            -RedirectStandardOutput $prepLog -RedirectStandardError "$prepLog.err" -ArgumentList @(
            '-NoProfile', '-ExecutionPolicy', 'Bypass',
            '-File', "`"$(Join-Path $PSScriptRoot 'build.ps1')`"", '-Prepare')
        $null = $prep.Handle
        Write-Host '  compiling the toolkit and the launcher alongside (build/prepare.log)'
    }
    $failed = @()
    foreach ($e in $procs) {
        $e.Proc.WaitForExit()
        if ($e.Proc.ExitCode -ne 0) { $failed += $e.Name }
    }
    if ($prep) {
        # Not an error of the lift: build.ps1 shows the same failure again.
        $prep.WaitForExit()
        if ($prep.ExitCode -ne 0) { Write-Host '  (compiling ahead failed, see build/prepare.log)' }
    }
    if ($failed) { throw "recompile failed for: $($failed -join ', ') (logs in build/pipeline/<target>)" }
    Write-Host ("Done in {0:N0}s. Next: ./scripts/build.ps1" -f $sw.Elapsed.TotalSeconds)
    return
}

# Per-XBE settings. Both disassemble every section, not just .text
# (--text-only): with it, the XDK sections of default.xbe (D3D, DSOUND) came
# out with functions cut short at their first forward jmp, and the branches
# past that point went to empty stubs -- DirectSound init then ran with a
# wrecked stack.
$targets = @{
    main = @{
        Xbe = 'game/default.xbe'; Seeds = 'config/seeds/main.json'
        Manual = 'src/manual_main.c'
    }
    driving = @{
        Xbe = 'game/Driving.xbe'; Seeds = 'config/seeds/driving.json'
        Manual = 'src/manual_driving.c'
    }
}
$stages = @('parse', 'disasm', 'identify', 'abi', 'lift')

# The tools run as modules from the toolkit root. Judged by exit code alone:
# they print warnings on stderr, and Windows PowerShell 5.1 turns any stderr
# line into an error under 'Stop'.
function Invoke-Tool([string]$target, [string]$name, [string[]]$toolArgs, [string]$log) {
    Write-Host "  ${target}: $name"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $ErrorActionPreference = 'Continue'
    # An empty input: the tools read none, and a Python that should ever open
    # its prompt instead ends at once rather than looping into the log.
    '' | & $pyExe @pyPre -m $name @toolArgs *> $log
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($code -ne 0) { throw "$name failed ($code), see $log" }
    Write-Host ("  ${target}: $name done ({0:N0}s)" -f $sw.Elapsed.TotalSeconds)
}

# One hash over everything identify, abi and lift read: the disasm output, the
# XBE, the hand-written functions, the toolkit's Python and this script (which
# holds the tools' arguments).
function Get-LiftInputHash([string[]]$files) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $sb = New-Object Text.StringBuilder
    foreach ($f in $files) {
        $rel = $f.Substring($root.Length)
        $null = $sb.AppendLine("$rel $((Get-FileHash -Algorithm SHA256 $f).Hash)")
    }
    $bytes = [Text.Encoding]::UTF8.GetBytes($sb.ToString())
    return [BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-', '')
}

# Make $dst hold exactly the files of $src, rewriting only those whose bytes
# differ: an unchanged chunk keeps its timestamp and is not recompiled, and a
# chunk the new lift no longer writes is removed rather than compiled twice.
function Sync-Directory([string]$src, [string]$dst) {
    New-Item -ItemType Directory -Force $dst | Out-Null
    $new = @{}
    foreach ($f in Get-ChildItem -File $src) { $new[$f.Name] = $f }
    $changed = 0
    foreach ($old in Get-ChildItem -File $dst) {
        if (-not $new.ContainsKey($old.Name)) { Remove-Item -Force $old.FullName; $changed++ }
    }
    foreach ($f in $new.Values) {
        $d = Join-Path $dst $f.Name
        if ((Test-Path $d) -and
            (Get-FileHash $d).Hash -eq (Get-FileHash $f.FullName).Hash) { continue }
        Copy-Item -Force $f.FullName $d
        $changed++
    }
    return $changed
}

function Invoke-Target([string]$name) {
    $t = $targets[$name]
    $xbe = Join-Path $root $t.Xbe
    if (-not (Test-Path $xbe)) {
        throw "No XBE at $xbe. Put your extracted disc in game/."
    }
    $work = Join-Path $root "build/pipeline/$name"
    $gen = Join-Path $root "src/recomp/$name/gen"
    $genNew = Join-Path $work 'gen_new'
    $seeds = Join-Path $root $t.Seeds
    $manual = Join-Path $root $t.Manual
    $disasm = Join-Path $work 'disasm'
    $funcid = Join-Path $work 'func_id'
    $abi = Join-Path $work 'abi'
    $analysis = Join-Path $work 'analysis.json'
    $stamp = Join-Path $work 'lift.stamp'
    New-Item -ItemType Directory -Force $work, $disasm, $funcid, $abi | Out-Null

    Write-Host "== $name ($($t.Xbe))"
    $run = $false
    $liftHash = $null
    foreach ($stage in $stages) {
        if ($stage -eq $From) { $run = $true }
        if (-not $run) { continue }

        # Coming from parse or disasm, the later stages only run when what
        # they read differs from the last successful lift.
        if (($stage -in 'identify', 'abi', 'lift') -and -not $liftHash) {
            $inputs = @($xbe, $manual, $PSCommandPath) +
                @('functions.json', 'labels.json', 'xrefs.json', 'strings.json' |
                    ForEach-Object { Join-Path $disasm $_ }) +
                @(Get-ChildItem -Recurse -File (Join-Path $toolkit 'tools') -Filter *.py |
                    Sort-Object FullName | ForEach-Object FullName)
            $liftHash = Get-LiftInputHash $inputs
            if (($From -in 'parse', 'disasm') -and (Test-Path $stamp) -and
                (Get-Content -Raw $stamp).Trim() -eq $liftHash -and
                (Test-Path (Join-Path $gen 'recomp_funcs.h'))) {
                Write-Host "  ${name}: identify/abi/lift inputs unchanged, skipped"
                break
            }
            # Gone until the lift succeeds, so a failed run is never skipped.
            if (Test-Path $stamp) { Remove-Item -Force $stamp }
        }

        $log = Join-Path $work "$stage.log"
        switch ($stage) {
            'parse' {
                Invoke-Tool $name 'tools.xbe_parser' @($xbe, '--json', $analysis) $log
            }
            'disasm' {
                Invoke-Tool $name 'tools.disasm' (@($xbe, '-o', $disasm,
                    '--seed-functions', $seeds, '--analysis-json', $analysis) + $disasmExtra) $log
            }
            'identify' {
                Invoke-Tool $name 'tools.func_id' @($xbe,
                    '--functions', (Join-Path $disasm 'functions.json'),
                    '--strings', (Join-Path $disasm 'strings.json'),
                    '--xrefs', (Join-Path $disasm 'xrefs.json'), '-o', $funcid) $log
            }
            'abi' {
                Invoke-Tool $name 'tools.abi_analysis' @($xbe, '--disasm-dir', $disasm,
                    '--func-id-dir', $funcid, '--output-dir', $abi) $log
            }
            'lift' {
                # Into a fresh staging directory, then only the changed files
                # into gen (see Sync-Directory).
                if (Test-Path $genNew) { Remove-Item -Recurse -Force $genNew }
                Invoke-Tool $name 'tools.recomp' @($xbe, '--all', '--split', '250',
                    '--gen-dir', $genNew, '-o', (Join-Path $work 'recomp'),
                    '--disasm-dir', $disasm, '--func-id-dir', $funcid, '--abi-dir', $abi,
                    '--exclude-manual', $manual) $log
                $changed = Sync-Directory $genNew $gen
                Write-Host "  ${name}: $changed generated file(s) changed"
                Set-Content -Encoding ascii $stamp $liftHash
            }
        }
    }
}

Push-Location $toolkit
try {
    Invoke-Target $Target
} finally {
    Pop-Location
}
Write-Host "Done ($Target). Next: ./scripts/build.ps1"
