<#
.SYNOPSIS
  dunstctl for Windows: control a running dunst over its named pipe.

.DESCRIPTION
  Port of the POSIX `dunstctl` shell script. Instead of dbus-send/busctl it
  writes one line per call to \\.\pipe\org.freedesktop.Notifications.<SessionId>
  (or $env:WNOTIFY_PIPE), see src/win32/dbus_win32.inc for the format.

.EXAMPLE
  .\dunstctl.ps1 close-all
  .\dunstctl.ps1 history | ConvertFrom-Json
  .\dunstctl.ps1 set-paused toggle
#>
param([Parameter(Position = 0)][string]$Command = 'help',
      [Parameter(Position = 1, ValueFromRemainingArguments = $true)][string[]]$Rest = @())

$ErrorActionPreference = 'Stop'
$DUNST = 'org.dunstproject.cmd0'
$PROP = 'org.freedesktop.DBus.Properties'

function Get-PipeName {
    if ($env:WNOTIFY_PIPE) { return ($env:WNOTIFY_PIPE -replace '^\\\\\.\\pipe\\', '') }
    "org.freedesktop.Notifications.$((Get-Process -Id $PID).SessionId)"
}

function Escape-Field([string]$s) {
    $s.Replace('\', '\\').Replace("`t", '\t').Replace("`n", '\n').Replace("`r", '\r')
}

function Unescape-Field([string]$s) {
    [regex]::Replace($s, '\\(.)', { param($m) switch ($m.Groups[1].Value) { 't' { "`t" } 'n' { "`n" } 'r' { "`r" } default { $m.Groups[1].Value } } })
}

function Invoke-Dunst([string[]]$fields) {
    $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', (Get-PipeName), [System.IO.Pipes.PipeDirection]::InOut)
    try { $pipe.Connect(2000) } catch { Write-Error 'dunst is not running (no notifications pipe)'; exit 1 }
    try {
        $line = (($fields | ForEach-Object { Escape-Field $_ }) -join "`t") + "`n"
        $bytes = [Text.Encoding]::UTF8.GetBytes($line)
        $pipe.Write($bytes, 0, $bytes.Length); $pipe.Flush()
        $reader = New-Object System.IO.StreamReader($pipe, [Text.Encoding]::UTF8)
        $reply = $reader.ReadLine()
    } finally { $pipe.Dispose() }
    $parts = @($reply -split "`t" | ForEach-Object { Unescape-Field $_ })
    if ($parts[0] -ne 'OK') { Write-Error ("dunst: " + ($parts | Select-Object -Skip 1) -join ' '); exit 1 }
    # ,@(...): PowerShell unrolls a function's output once; without the extra
    # wrapper a one-field reply becomes a bare string and [0] yields its first char.
    return , @($parts | Select-Object -Skip 1)
}

function Call([string]$method, [string]$arg = '()') { Invoke-Dunst @("$DUNST.$method", $arg) | Out-Null }
function Prop-Get([string]$name) { (Invoke-Dunst @("$PROP.Get", $name))[0] -replace '^(uint32|boolean) ', '' }
function Prop-Set([string]$name, [string]$value) { Invoke-Dunst @("$PROP.Set", $name, $value) | Out-Null }

switch ($Command) {
    'action'          { Call 'NotificationAction' "(uint32 $(if ($Rest) { $Rest[0] } else { 0 }),)" }
    'close'           { if ($Rest) { Invoke-Dunst @('CloseNotification', $Rest[0]) | Out-Null } else { Call 'NotificationCloseLast' } }
    'close-all'       { Call 'NotificationCloseAll' }
    'context'         { Call 'ContextMenuCall' }
    'count' {
        if ($Rest) { Prop-Get "$($Rest[0])Length" }
        else {
            "              Waiting: $(Prop-Get waitingLength)"
            "  Currently displayed: $(Prop-Get displayedLength)"
            "              History: $(Prop-Get historyLength)"
        }
    }
    'history'         { (Invoke-Dunst @("json:$DUNST.NotificationListHistory"))[0] }
    'history-clear'   { Call 'NotificationClearHistory' }
    'history-pop'     { if ($Rest) { Call 'NotificationPopHistory' "(uint32 $($Rest[0]),)" } else { Call 'NotificationShow' } }
    'history-rm'      { if (-not $Rest) { Write-Error 'history-rm needs an ID'; exit 1 }; Call 'NotificationRemoveFromHistory' "(uint32 $($Rest[0]),)" }
    'is-paused' {
        $p = Prop-Get paused
        if ($Rest -contains '-e' -or $Rest -contains '--exit-code') { exit ([int]($p -ne 'true')) } else { $p }
    }
    'set-paused' {
        $v = if ($Rest[0] -eq 'toggle') { if ((Prop-Get paused) -eq 'true') { 'false' } else { 'true' } } else { $Rest[0] }
        if ($v -notin 'true', 'false') { Write-Error 'set-paused takes true|false|toggle'; exit 1 }
        Prop-Set paused $v
    }
    'get-pause-level' { Prop-Get pauseLevel }
    'set-pause-level' { Prop-Set pauseLevel "uint32 $($Rest[0])" }
    'rule' {
        $state = @{ enable = 1; disable = 0; toggle = 2 }[$Rest[1]]
        if ($null -eq $state) { Write-Error 'rule NAME enable|disable|toggle'; exit 1 }
        Call 'RuleEnable' "('$($Rest[0])', $state)"
    }
    'rules'           { (Invoke-Dunst @("json:$DUNST.RuleList"))[0] }
    'reload'          { $list = ($Rest -split '[ ,]' | Where-Object { $_ } | ForEach-Object { "'" + ($_ -replace "'", "\'") + "'" }) -join ', '; Call 'ConfigReload' "(@as [$list],)" }
    'debug'           { Call 'Ping'; "pipe: \\.\pipe\$(Get-PipeName)"; Invoke-Dunst @('GetServerInformation') }
    default {
        @'
Usage: dunstctl <command> [parameters]
Commands:
  action [POS]                      Default action / context menu of notification at POS
  close [ID]                        Close the last notification or the one with ID
  close-all                         Close all the notifications
  context                           Open context menu
  count [displayed|history|waiting] Show the number of notifications
  history                           Display notification history (JSON)
  history-clear                     Delete all notifications from history
  history-pop [ID]                  Pop the latest (or ID) notification from history
  history-rm ID                     Remove the notification with ID from history
  is-paused [-e|--exit-code]        Check if pause level is greater than 0
  set-paused true|false|toggle      Set the pause status
  get-pause-level                   Get the current pause level
  set-pause-level LEVEL             Set the pause level
  rule NAME enable|disable|toggle   Enable or disable a rule by its name
  rules                             Display configured rules (JSON)
  reload [dunstrc ...]              Reload the settings of the running instance
  debug                             Print debugging information
  help                              Show help
'@
    }
}
