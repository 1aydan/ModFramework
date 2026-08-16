# Copyright (c) 2026. Licensed for use in your own projects.
<#
.SYNOPSIS
	Runs the ModFramework automation suites headless.

.DESCRIPTION
	Compiling proves the code is well formed. This proves it behaves. Run it after any change to the
	runtime - the suites cover the state machine, dependency resolution, manifest parsing, permissions,
	config, save isolation, package reading and multiplayer validation.

	Uses the throwaway harness project so it neither disturbs the sample game nor fights an open
	editor for module DLL locks.

	NOTE ON INVOCATION: -ExecCmds's value contains spaces and a semicolon. Passing that through
	PowerShell's native-command argument handling mangles it - the editor never sees the command and
	you get a log full of unrelated build chatter. Start-Process with an explicit argument array is
	the reliable way to hand it over intact.

.PARAMETER Filter
	Test name prefix. Defaults to every ModFramework suite. e.g. -Filter ModFramework.Lifecycle
#>
[CmdletBinding()]
param(
	[string]$Filter = "ModFramework"
)

$Editor  = "F:\SelfProjects\Unreal\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
$Project = "F:\SelfProjects\Unreal\_ModHarness\ModHarness.uproject"
$Log     = "F:\SelfProjects\Unreal\_ModHarness\Saved\Logs\AutomationRun.log"
$StdOut  = "F:\SelfProjects\Unreal\_ModHarness\tests-stdout.log"

if (-not (Test-Path $Project)) { throw "No harness project at '$Project'. See .dev/build-harness.ps1." }

New-Item -ItemType Directory -Force (Split-Path $Log) | Out-Null
Remove-Item $Log, $StdOut -ErrorAction SilentlyContinue

$Arguments = @(
	"`"$Project`""
	"-ExecCmds=Automation RunTests $Filter"
	"-TestExit=Automation Test Queue Empty"
	"-ReportExportPath=`"F:\SelfProjects\Unreal\_ModHarness\Saved\AutomationReport`""
	"-unattended"
	"-nullrhi"
	"-nosplash"
	"-NoSound"
	"-abslog=`"$Log`""
)

Write-Host "Running: Automation RunTests $Filter"
$Process = Start-Process -FilePath $Editor -ArgumentList $Arguments -Wait -PassThru `
	-RedirectStandardOutput $StdOut -NoNewWindow

$Lines = @()
if (Test-Path $Log)    { $Lines += Get-Content $Log }
if (Test-Path $StdOut) { $Lines += Get-Content $StdOut }

if ($Lines.Count -eq 0)
{
	Write-Host "No log produced. Editor exit code $($Process.ExitCode)." -ForegroundColor Red
	return
}

$Results = $Lines | Select-String -Pattern "LogAutomationController: (Display|Warning|Error):" |
	ForEach-Object { $_.Line.Trim() }

$Failures = $Results | Where-Object { $_ -match "Fail|Error:" }
$Summary  = $Results | Where-Object { $_ -match "Test Completed|tests? (ran|passed|failed)|Success|Total" } |
	Select-Object -Last 8

Write-Host "=== exit $($Process.ExitCode) ==="
if ($Summary) { $Summary }

# "No failures" is only meaningful if tests actually ran. An empty automation log means the
# controller never engaged - a wrong filter, a mangled -ExecCmds, a missing module - and reporting
# that as success is worse than reporting a failure, because it looks like passing coverage.
if ($Results.Count -eq 0)
{
	Write-Host ""
	Write-Host "NO TESTS RAN. The automation controller produced no output at all." -ForegroundColor Red
	Write-Host "This is NOT a pass. Check the filter ('$Filter') and that the log below mentions automation:"
	Write-Host "  $Log"
	exit 1
}

if ($Failures)
{
	Write-Host ""
	Write-Host "=== $($Failures.Count) failure lines ===" -ForegroundColor Red
	$Failures | Select-Object -First 40
	exit 1
}

Write-Host ""
Write-Host "$($Results.Count) automation lines, no failures." -ForegroundColor Green
