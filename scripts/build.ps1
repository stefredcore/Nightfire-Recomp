<#
.SYNOPSIS
    Build nightfire.exe and nightfire_driving.exe.

.DESCRIPTION
    Configures build/ with CMake (first run only) and builds both executables.
    The generated C has to exist first: run scripts/recompile.ps1.

    With a toolchain\ folder beside the project (a player's install, see
    installer\) it builds with the Zig, CMake and Ninja in it; without one,
    with Visual Studio.

.PARAMETER Config
    Release (default) or Debug.

.PARAMETER Prepare
    Configure and compile what needs no generated code (the toolkit and the
    launcher), at below-normal priority. scripts/recompile.ps1 runs this
    beside the lift, so the cores the lift leaves free are not idle; the full
    build afterwards then only compiles the game.
#>
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',
    [switch]$Prepare
)

$ErrorActionPreference = 'Stop'
# A click into the window would otherwise pause the build until Enter.
. (Join-Path $PSScriptRoot 'quickedit.ps1')
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'

if ($Prepare) {
    # The compilers inherit this, so the lift beside it keeps the CPU first.
    (Get-Process -Id $PID).PriorityClass = 'BelowNormal'
} else {
    $missing = @('main', 'driving') | Where-Object {
        -not (Test-Path (Join-Path $root "src/recomp/$_/gen/recomp_funcs.h"))
    }
    if ($missing) {
        throw "Generated code missing for: $($missing -join ', '). Run scripts/recompile.ps1 first."
    }

    # No enhancement may run in the original mode (scripts/check_original.ps1).
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'check_original.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Original mode check failed (see above).' }
}

# The tools: toolchain\ beside the project when it is there -- a player's
# install (installer\) carries Zig, CMake and Ninja in it and builds with
# Zig's Clang (cmake/zig-windows.cmake) -- else Visual Studio.
$tc = Join-Path $root 'toolchain'
$zig = Join-Path $tc 'zig\zig.exe'
$useZig = Test-Path $zig
if ($useZig) {
    $cmake = Join-Path $tc 'cmake\bin\cmake.exe'
    $ninja = Join-Path $tc 'ninja\ninja.exe'
    foreach ($t in $cmake, $ninja) {
        if (-not (Test-Path $t)) { throw "$t is missing. Run the installer again." }
    }
    # Zig's caches (the MinGW runtime it builds once) inside the install, not
    # in %LOCALAPPDATA%: deleting the folder removes everything.
    $env:ZIG_GLOBAL_CACHE_DIR = Join-Path $tc 'zig-cache'
    $env:ZIG_LOCAL_CACHE_DIR = Join-Path $tc 'zig-cache'
} else {
    # cmake from PATH, else the one Visual Studio ships with: outside a
    # Developer PowerShell that one is not on PATH.
    $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
    if (-not $cmake) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path $vswhere) {
            $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if ($vs) {
                $c = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
                if (Test-Path $c) { $cmake = $c }
            }
        }
    }
    if (-not $cmake) {
        throw 'cmake not found. Install Visual Studio with "Desktop development with C++" (includes CMake) or CMake itself.'
    }
}

# cmake writes progress and warnings to stderr; only the exit code counts.
$ErrorActionPreference = 'Continue'
# A build folder configured for the other toolchain is configured again.
$cache = Join-Path $build 'CMakeCache.txt'
if (Test-Path $cache) {
    $wasNinja = [bool](Select-String -Path $cache -Pattern '^CMAKE_GENERATOR:INTERNAL=Ninja$' -Quiet)
    if ($wasNinja -ne $useZig) {
        Remove-Item -Force $cache
        Remove-Item -Recurse -Force (Join-Path $build 'CMakeFiles') -ErrorAction SilentlyContinue
    }
}
# The game executables are in the build unless this is -Prepare.
$only = if ($Prepare) { 'ON' } else { 'OFF' }
if ((Test-Path $cache) -and -not (Select-String -Path $cache -Quiet `
        -Pattern "^NIGHTFIRE_TOOLKIT_ONLY:BOOL=$only`$")) {
    # Configured before with the other setting: switched over in place, the
    # compiler checks and everything already compiled stay.
    & $cmake -S $root -B $build "-DNIGHTFIRE_TOOLKIT_ONLY=$only"
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
}
if (-not (Test-Path $cache)) {
    if ($useZig) {
        # One configuration (Ninja), the programs in build\<Config>\ as with
        # Visual Studio, where the launcher and the installer look for them.
        $fwd = { param($p) $p -replace '\\', '/' }
        & $cmake -S $root -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Config" "-DNIGHTFIRE_TOOLKIT_ONLY=$only" `
            "-DCMAKE_MAKE_PROGRAM=$(& $fwd $ninja)" `
            "-DCMAKE_TOOLCHAIN_FILE=$(& $fwd (Join-Path $root 'cmake\zig-windows.cmake'))" `
            "-DZIG_EXE=$(& $fwd $zig)" `
            "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$(& $fwd (Join-Path $build $Config))"
    } else {
        & $cmake -S $root -B $build -A x64 "-DNIGHTFIRE_TOOLKIT_ONLY=$only"
    }
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
}
if ($Prepare) {
    # Half the logical processors, less one: the lift's disassembly beside it
    # is what the install waits for, and on a CPU busy to the last core its
    # threads lose their boost clock and share their cores. With fewer jobs
    # this still ends well before the disassembly does.
    $jobs = [Math]::Max(1, [int][Math]::Floor([Environment]::ProcessorCount / 2) - 1)
    & $cmake --build $build --config $Config --parallel $jobs
} else {
    & $cmake --build $build --config $Config --parallel
}
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
if ($Prepare) {
    Write-Host 'Prepared: the toolkit and the launcher are compiled.'
    return
}

# The launcher beside the game folder, the one file a player starts. Not named
# nightfire.exe: the launcher takes a process of that name for the game. A
# running one cannot be overwritten, but renamed (it deletes the .old itself).
$launcher = Join-Path $root 'Nightfire Launcher.exe'
if (Test-Path $launcher) {
    Remove-Item "$launcher.old" -ErrorAction SilentlyContinue
    try { Copy-Item (Join-Path $build "$Config\nightfire_launcher.exe") $launcher -ErrorAction Stop }
    catch {
        Move-Item $launcher "$launcher.old"
        Copy-Item (Join-Path $build "$Config\nightfire_launcher.exe") $launcher
    }
} else {
    Copy-Item (Join-Path $build "$Config\nightfire_launcher.exe") $launcher
}

Write-Host "Done: $build\$Config\nightfire.exe + nightfire_driving.exe. Start with 'Nightfire Launcher.exe'."
