<#
.SYNOPSIS
    Builds installer\dist\Nightfire-Setup.exe, the installer for players.

.DESCRIPTION
    1. Packs the payload into installer\obj:
         project.zip   the project at HEAD (git archive: tracked files only,
                       so no game files, generated code, build output or local
                       settings)
         toolkit.zip   the toolkit at the commit HEAD points the submodule at,
                       under external\xboxrecomp\
         manifest.txt  both commits, the repository and branch updates come
                       from, and the date; written to install-info.txt
         toolchain.zip the build tools (Zig, Python + capstone, CMake, Ninja)
                       and their licences, as toolchain\ of an install;
                       downloaded once into installer\cache\dl, each checked
                       against its published SHA-256
    2. Builds installer\CMakeLists.txt with the payload in its resources.

    Only committed work goes in: commit (and push) first. Uncommitted changes
    are reported and left out.
#>
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',
    # For a test install before committing: the working trees as they are
    # (tracked and new, not ignored, files), versions marked +modified.
    [switch]$WorkingTree
)

$ErrorActionPreference = 'Stop'
$installer = $PSScriptRoot
$root = Split-Path -Parent $installer
$toolkit = Join-Path $root 'external\xboxrecomp'
$obj = Join-Path $installer 'obj'
$build = Join-Path $installer 'build'
New-Item -ItemType Directory -Force $obj | Out-Null

# git writes progress to stderr; only the exit code counts.
function Invoke-Git([string[]]$GitArgs) {
    $ErrorActionPreference = 'Continue'
    $out = & git @GitArgs 2>&1
    if ($LASTEXITCODE -ne 0) { throw "git $($GitArgs -join ' ') failed: $out" }
    return $out
}

$project = (Invoke-Git @('-C', $root, 'rev-parse', '--short', 'HEAD')).Trim()
# The toolkit commit HEAD names; with -WorkingTree the one checked out (HEAD
# may not hold the submodule yet, as in a fresh checkout before its first
# commit).
$tkFull = if ($WorkingTree) {
    (Invoke-Git @('-C', $toolkit, 'rev-parse', 'HEAD')).Trim()
} else {
    (Invoke-Git @('-C', $root, 'rev-parse', 'HEAD:external/xboxrecomp')).Trim()
}
$tk = $tkFull.Substring(0, 7)
foreach ($repo in $root, $toolkit) {
    $dirty = Invoke-Git @('-C', $repo, 'status', '--porcelain', '--untracked-files=no')
    if ($dirty -and -not $WorkingTree) {
        Write-Host "Note: uncommitted changes in $repo are not in the installer." -ForegroundColor Yellow
    }
}

if ($WorkingTree) {
    # The files git would commit (tracked plus new, not ignored), as they are.
    $tar = Join-Path $env:SystemRoot 'System32\tar.exe'
    foreach ($p in @(@($root, 'project.zip', ''), @($toolkit, 'toolkit.zip', 'external/xboxrecomp/'))) {
        $list = Join-Path $obj "$($p[1]).list"
        $files = Invoke-Git @('-C', $p[0], 'ls-files', '--cached', '--others', '--exclude-standard') |
            Where-Object { $_ -notlike '.claude/*' -and (Test-Path -LiteralPath (Join-Path $p[0] $_) -PathType Leaf) } |
            ForEach-Object { $p[2] + $_ }
        [IO.File]::WriteAllLines($list, [string[]]$files)
        $zip = Join-Path $obj $p[1]
        if (Test-Path $zip) { Remove-Item $zip }
        & $tar -a -cf $zip -C $root -T $list
        if ($LASTEXITCODE -ne 0) { throw "Packing $($p[1]) failed." }
        Remove-Item $list
    }
    $project += '+modified'
    $tk += '+modified'
} else {
    Invoke-Git @('-C', $root, 'archive', '--format=zip', '-o', (Join-Path $obj 'project.zip'), 'HEAD') | Out-Null
    Invoke-Git @('-C', $toolkit, 'archive', '--format=zip', '--prefix=external/xboxrecomp/',
                 '-o', (Join-Path $obj 'toolkit.zip'), $tkFull) | Out-Null
}
# Where an install's updates come from: the launcher checks this repository
# and branch, and its first update turns the folder into a checkout of them.
$repo = (Invoke-Git @('-C', $root, 'remote', 'get-url', 'origin')).Trim() -replace '://[^/@]+@', '://'
$branch = (Invoke-Git @('-C', $root, 'rev-parse', '--abbrev-ref', 'HEAD')).Trim()
$manifest = "[install]`r`nproject=$project`r`ntoolkit=$tk`r`nrepo=$repo`r`nbranch=$branch`r`n" +
            "packed=$(Get-Date -Format 'yyyy-MM-dd HH:mm')`r`n"
[IO.File]::WriteAllText((Join-Path $obj 'manifest.txt'), $manifest)
Write-Host ("Payload: project {0} ({1:N1} MB), toolkit {2} ({3:N1} MB)" -f $project,
    ((Get-Item (Join-Path $obj 'project.zip')).Length / 1MB), $tk,
    ((Get-Item (Join-Path $obj 'toolkit.zip')).Length / 1MB))

# ── The build tools ──────────────────────────────────────────────
# Everything a player's PC needs to build the game, so it needs no Visual
# Studio and no Python of its own: Zig (its Clang, linker and the MinGW-w64
# Windows headers), the embeddable Python with capstone for the lift, CMake
# and Ninja. Each at a fixed version from its official download, checked
# against the SHA-256 published there, and shipped unmodified; their licence
# texts go into toolchain\licenses. capstone and Python stay at the versions
# the lift has been checked with: a different capstone may decode differently.
$tools = @(
    @{ Name = 'zig'; Version = '0.17.1'; Licence = 'MIT (LLVM: Apache-2.0 WITH LLVM-exception; MinGW-w64: see COPYING)'
       Url = 'https://ziglang.org/download/0.17.1/zig-x86_64-windows-0.17.1.zip'
       Sha = 'b03f477a1943673e02aca0f570e8030dffd536ce1f45cbc4d3326ec31159562e'; Top = 'zig-x86_64-windows-0.17.1' }
    @{ Name = 'python'; Version = '3.14.7 embeddable'; Licence = 'PSF License (and the licences listed in it)'
       Url = 'https://www.python.org/ftp/python/3.14.7/python-3.14.7-embed-amd64.zip'
       Sha = 'd297e5ff019966817ad8502465176139f2d3d840fa4ed84b13bed399a6ab1f15' }
    @{ Name = 'capstone'; Version = '5.0.7'; Licence = 'BSD-3-Clause'; File = 'capstone.whl'
       Url = 'https://files.pythonhosted.org/packages/70/39/2138d890a8e827636b9de9924fbc8527fe83e38b6d26605b30ac55e30ebe/capstone-5.0.7-py3-none-win_amd64.whl'
       Sha = '4ab8bcb7da8f221ff45926ca168ca33e76f7237d06fbf3c10780002faa2670e1' }
    @{ Name = 'cmake'; Version = '4.4.3'; Licence = 'BSD-3-Clause'
       Url = 'https://github.com/Kitware/CMake/releases/download/v4.4.3/cmake-4.4.3-windows-x86_64.zip'
       Sha = '4d52ebab7193a698651639ed80d8d04fd903358843572cf44c7fd234cb7c26ab'; Top = 'cmake-4.4.3-windows-x86_64' }
    @{ Name = 'ninja'; Version = '1.13.2'; Licence = 'Apache-2.0'
       Url = 'https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip'
       Sha = '07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65' }
    @{ Name = 'ninja-licence'; File = 'ninja-COPYING.txt'
       Url = 'https://raw.githubusercontent.com/ninja-build/ninja/v1.13.2/COPYING'
       Sha = 'eb7e9ab9690124c5c9f42bdc81383d886a3dede26345b6ed15bbad7caf81f7ea' }
)
$dl = Join-Path $installer 'cache\dl'
New-Item -ItemType Directory -Force $dl | Out-Null
$ProgressPreference = 'SilentlyContinue'
foreach ($t in $tools) {
    $t.Path = Join-Path $dl $(if ($t.File) { $t.File } else { "$($t.Name).zip" })
    $ok = (Test-Path $t.Path) -and (Get-FileHash $t.Path).Hash.ToLower() -eq $t.Sha
    if (-not $ok) {
        Write-Host "Downloading $($t.Name) $($t.Version)"
        Invoke-WebRequest -UseBasicParsing $t.Url -OutFile $t.Path
        $got = (Get-FileHash $t.Path).Hash.ToLower()
        if ($got -ne $t.Sha) {
            Remove-Item $t.Path
            throw "$($t.Name): SHA-256 $got is not the published $($t.Sha). Not packed."
        }
    }
}

# toolchain\ as an install has it, rebuilt when the tool list changes.
$stage = Join-Path $obj 'toolchain'
$tcZip = Join-Path $obj 'toolchain.zip'
$stamp = Join-Path $obj 'toolchain.stamp'
$key = ($tools | ForEach-Object { "$($_.Name) $($_.Sha)" }) -join "`n"
if (-not (Test-Path $tcZip) -or -not (Test-Path $stamp) -or (Get-Content -Raw $stamp) -ne $key) {
    Write-Host 'Packing the build tools'
    $tar = Join-Path $env:SystemRoot 'System32\tar.exe'
    $tmp = Join-Path $obj 'toolchain_unpack'
    foreach ($d in $stage, $tmp) { if (Test-Path $d) { Remove-Item -Recurse -Force $d } }
    New-Item -ItemType Directory -Force $stage, $tmp | Out-Null
    function Get-Tool([string]$name) { $tools | Where-Object { $_.Name -eq $name } }
    foreach ($n in 'zig', 'python', 'cmake', 'ninja') {
        $t = Get-Tool $n
        $into = Join-Path $tmp $n
        New-Item -ItemType Directory -Force $into | Out-Null
        & $tar -xf $t.Path -C $into
        if ($LASTEXITCODE -ne 0) { throw "Unpacking $n failed." }
        # The zips of Zig and CMake hold one top folder; toolchain\<name> is its content.
        $from = if ($t.Top) { Join-Path $into $t.Top } else { $into }
        Move-Item $from (Join-Path $stage $n)
    }
    $py = Join-Path $stage 'python'
    New-Item -ItemType Directory -Force (Join-Path $py 'Lib\site-packages') | Out-Null
    & $tar -xf (Get-Tool 'capstone').Path -C (Join-Path $py 'Lib\site-packages')
    if ($LASTEXITCODE -ne 0) { throw 'Unpacking capstone failed.' }
    # The embedded Python reads only the paths in its ._pth: its library, the
    # site-packages with capstone, and the toolkit, from toolchain\python.
    [IO.File]::WriteAllText((Join-Path $py 'python314._pth'),
        "python314.zip`r`n.`r`nLib\site-packages`r`n..\..\external\xboxrecomp`r`n")

    $lic = Join-Path $stage 'licenses'
    New-Item -ItemType Directory -Force $lic | Out-Null
    $copies = @(
        @('zig\LICENSE', 'Zig-LICENSE.txt'),
        @('zig\lib\libcxx\LICENSE.TXT', 'LLVM-LICENSE.txt'),
        @('zig\lib\libc\mingw\COPYING', 'MinGW-w64-COPYING.txt'),
        @('python\LICENSE.txt', 'Python-LICENSE.txt'),
        @('python\Lib\site-packages\capstone-5.0.7.dist-info\LICENSE.TXT', 'capstone-LICENSE.txt'),
        @('cmake\doc\cmake\LICENSE.rst', 'CMake-LICENSE.rst')
    )
    foreach ($c in $copies) {
        $src = Join-Path $stage $c[0]
        if (-not (Test-Path $src)) { throw "Licence text missing: $src" }
        Copy-Item $src (Join-Path $lic $c[1])
    }
    Copy-Item (Get-Tool 'ninja-licence').Path (Join-Path $lic 'Ninja-COPYING.txt')
    $readme = @('The build tools in this folder, each unmodified from its official download:', '')
    foreach ($t in $tools | Where-Object { $_.Version }) {
        $readme += "  $($t.Name) $($t.Version) -- $($t.Licence)"
        $readme += "    $($t.Url)"
        $readme += "    SHA-256 $($t.Sha)"
    }
    $readme += ''
    $readme += 'Their licence texts are in this folder. The toolkit''s (MIT, with LGPL-2.1 parts'
    $readme += 'from xemu) are in external\xboxrecomp (LICENSE, NOTICE, LICENSES\).'
    [IO.File]::WriteAllLines((Join-Path $lic 'README.txt'), $readme)
    Remove-Item -Recurse -Force $tmp

    if (Test-Path $tcZip) { Remove-Item $tcZip }
    & $tar -a -cf $tcZip -C $obj toolchain
    if ($LASTEXITCODE -ne 0) { throw 'Packing toolchain.zip failed.' }
    Set-Content -NoNewline -Encoding ascii $stamp $key
}
Write-Host ("Build tools: {0:N0} MB" -f ((Get-Item $tcZip).Length / 1MB))

# cmake from PATH, else Visual Studio's (as scripts\build.ps1).
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
if (-not $cmake) { throw 'cmake not found.' }

$ErrorActionPreference = 'Continue'
& $cmake -S $installer -B $build -A x64 -DPAYLOAD_DIR="$($obj -replace '\\', '/')"
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
& $cmake --build $build --config $Config
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
$ErrorActionPreference = 'Stop'

$dist = Join-Path $installer 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
$exe = Join-Path $dist 'Nightfire-Setup.exe'
Copy-Item (Join-Path $build "$Config\Nightfire-Setup.exe") $exe -Force
Write-Host ("Built {0} ({1:N1} MB)" -f $exe, ((Get-Item $exe).Length / 1MB)) -ForegroundColor Green
