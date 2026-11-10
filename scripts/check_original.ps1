<#
.SYNOPSIS
    Check that every enhancement stays out of the original mode.

.DESCRIPTION
    One build plays both modes (NF_MODE, see src/main.c). Every function
    src/manual_main.c and src/manual_driving.c define for the title
    (sub_XXXXXXXX, sub_XXXXXXXX_enter, ...) has to be one of two kinds:

      - a fix that makes the game play as on the console: it runs in both
        modes and says so with a comment "both modes: <why>" in its body;
      - an enhancement: it calls nf_enhanced(), and before that call writes
        nothing -- no guest memory (MEM*(...) = ...), no register (g_...),
        no variable of its own (s_...), and calls no function but the
        title's own body (sub_..._gen) and the memory reads.

    A function that is neither stops the build with its name. This reads the
    source only; it does not start the game. It looks at the text, so it
    catches a forgotten switch, not every way code can go wrong.

    build.ps1 runs it before compiling. On its own:
        powershell -ExecutionPolicy RemoteSigned -File scripts\check_original.ps1
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$files = @('src/manual_main.c', 'src/manual_driving.c')

# Comments to blanks of the same length, newlines kept, so positions in the
# stripped text are positions in the file.
function Remove-Comments([string]$text) {
    [regex]::Replace($text, '/\*[\s\S]*?\*/|//[^\n]*', {
        param($m) [regex]::Replace($m.Value, '[^\n]', ' ')
    })
}

# The statements before nf_enhanced() may only read and call the title.
function Get-WriteBeforeSwitch([string]$code) {
    foreach ($stmt in ($code -split '[;{}]')) {
        $s = $stmt.Trim()
        if (-not $s) { continue }
        if ($s -match '^MEM\w*\s*\(' -and $s -match '[^=!<>]=(?!=)') { return $s }
        if ($s -match '^(\+\+|--)?\s*(g|s)_\w+\s*(\+\+|--|[-+*/|&^]?=(?!=))') { return $s }
        if ($s -match '(\+\+|--)\s*(g|s)_\w+') { return $s }
        foreach ($call in [regex]::Matches($s, '\b([A-Za-z_]\w*)\s*\(')) {
            $name = $call.Groups[1].Value
            if ($name -match '^(MEM\w*|sub_\w+_gen|nf_enhanced|if|while|for|switch|return|sizeof|fabsf|fabs)$') { continue }
            return $s
        }
    }
    return $null
}

$errors = @()
$gated = 0
$both = 0
foreach ($rel in $files) {
    $path = Join-Path $root $rel
    $raw = [IO.File]::ReadAllText($path)
    $code = Remove-Comments $raw
    $defs = [regex]::Matches($code,
        '(?m)^(?:static\s+)?void\s+(sub_[0-9A-Fa-f]{8}\w*)\s*\(\s*void\s*\)\s*\{')
    foreach ($d in $defs) {
        $name = $d.Groups[1].Value
        $open = $d.Index + $d.Length - 1
        $depth = 0
        $end = -1
        for ($i = $open; $i -lt $code.Length; $i++) {
            if ($code[$i] -eq '{') { $depth++ }
            elseif ($code[$i] -eq '}') { $depth--; if ($depth -eq 0) { $end = $i; break } }
        }
        if ($end -lt 0) { $errors += "${rel}: $name has no closing brace"; continue }
        $bodyRaw = $raw.Substring($open + 1, $end - $open - 1)
        $body = $code.Substring($open + 1, $end - $open - 1)
        $line = ($raw.Substring(0, $d.Index) -split "`n").Count

        if ($bodyRaw -match 'both modes:') { $both++; continue }
        $at = $body.IndexOf('nf_enhanced()')
        if ($at -lt 0) {
            $errors += "${rel}:${line}: $name asks no nf_enhanced() and is not marked `"both modes:`""
            continue
        }
        $w = Get-WriteBeforeSwitch $body.Substring(0, $at)
        if ($w) {
            $errors += "${rel}:${line}: $name acts before nf_enhanced(): $(($w -replace '\s+', ' '))"
            continue
        }
        $gated++
    }
}

if ($errors) {
    Write-Host 'Original mode check FAILED -- an enhancement would run in the original mode:' -ForegroundColor Red
    $errors | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    Write-Host 'Ask nf_enhanced() first, or mark a console fix with a comment "both modes: <why>".'
    exit 1
}
Write-Host "Original mode check: $gated enhancements behind the switch, $both fixes in both modes."
