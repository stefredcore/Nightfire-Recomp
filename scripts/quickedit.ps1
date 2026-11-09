<#
.SYNOPSIS
    Turn the console's QuickEdit mode off while the calling script runs.

.DESCRIPTION
    With QuickEdit on, one click into the console window starts a selection, and
    Windows then suspends every process that writes to that window until Enter or
    Esc is pressed. A build that is quiet for minutes looks hung, while it is only
    waiting for the selection to end.

    Dot-source it (. $PSScriptRoot\quickedit.ps1) at the top of a long script.
    PowerShell puts the console mode back the way it found it when it exits, so
    the change lasts exactly as long as the script and needs no undoing; called
    as a separate process it would be reverted at once.
#>

if (-not ('Nightfire.Console' -as [type])) {
    Add-Type -Namespace Nightfire -Name Console -MemberDefinition @'
[DllImport("kernel32.dll")] public static extern System.IntPtr GetStdHandle(int which);
[DllImport("kernel32.dll")] public static extern bool GetConsoleMode(System.IntPtr h, out uint mode);
[DllImport("kernel32.dll")] public static extern bool SetConsoleMode(System.IntPtr h, uint mode);
'@
}

$quickEditHandle = [Nightfire.Console]::GetStdHandle(-10)    # STD_INPUT_HANDLE
$quickEditMode = [uint32]0
if ([Nightfire.Console]::GetConsoleMode($quickEditHandle, [ref]$quickEditMode)) {
    # Clear ENABLE_QUICK_EDIT_MODE (0x40); ENABLE_EXTENDED_FLAGS (0x80) makes it stick.
    [void][Nightfire.Console]::SetConsoleMode($quickEditHandle,
        [uint32](($quickEditMode -band (-bnot 0x40)) -bor 0x80))
}
