# Copyright (c) 2026. Licensed for use in your own projects.
<#
.SYNOPSIS
	Compiles the plugins against a throwaway host project.

.DESCRIPTION
	A UE plugin needs a host project to build. This uses a minimal one at F:\SelfProjects\Unreal\_ModHarness
	that junctions both plugins in, so the plugins can be compiled without touching the sample
	project — which matters when the editor is open on it, since building fights the editor for
	module DLL locks.

	WHY THE HARNESS LIVES THERE AND NOT IN A TEMP SCRATCH DIRECTORY: UnrealBuildTool refuses to
	build when any action path exceeds 260 characters (Windows MAX_PATH), and intermediate paths
	are long:
		<harness>\Plugins\ModFramework\Intermediate\Build\Win64\x64\UnrealEditor\Development\ModFramework\<file>.cpp.dep.json
	is ~123 characters on its own. A deep scratch path blows the limit before a single file is
	compiled, and the failure looks like a compile error but is not one. Keep the harness root short.

.PARAMETER Target
	UBT target name. Defaults to ModHarnessEditor.

.PARAMETER Clean
	Delete the plugins' Intermediate folders first.
#>
[CmdletBinding()]
param(
	[string]$Target = "ModHarnessEditor",
	[switch]$Clean
)

$Harness = "F:\SelfProjects\Unreal\_ModHarness"
$Project = Join-Path $Harness "ModHarness.uproject"
$Build   = "F:\SelfProjects\Unreal\UE_5.8\Engine\Build\BatchFiles\Build.bat"
$Log     = Join-Path $Harness "build.log"

if (-not (Test-Path $Project))
{
	throw "No harness project at '$Project'. See the description above for why it must live at a short path."
}

if ($Clean)
{
	foreach ($p in @("ModFramework", "GameModSDK"))
	{
		$i = Join-Path $Harness "Plugins\$p\Intermediate"
		if (Test-Path $i) { Remove-Item -Recurse -Force $i; Write-Host "cleaned $p" }
	}
}

& $Build $Target Win64 Development -project="$Project" -waitmutex -NoHotReload 2>&1 |
	Tee-Object -FilePath $Log | Out-Null
$Code = $LASTEXITCODE

$Lines = Get-Content $Log

# Real diagnostics only. Plain "error" matches far too much UBT chatter.
$Diagnostics = $Lines | Select-String -Pattern 'error [A-Z]+\d+|error LNK|: error:|LogInit: Error|Error: |fatal error' |
	ForEach-Object { $_.Line.Trim() } | Select-Object -Unique

Write-Host "=== exit $Code ==="
if ($Diagnostics)
{
	Write-Host "=== $($Diagnostics.Count) diagnostics ==="
	$Diagnostics | Select-Object -First 100
}
elseif ($Code -ne 0)
{
	# Nothing matched: the failure is probably structural (path length, missing module, bad rules).
	Write-Host "=== no compiler diagnostics matched; last 25 log lines ==="
	$Lines | Select-Object -Last 25 | ForEach-Object { $_.TrimEnd() }
}
else
{
	Write-Host "BUILD SUCCEEDED"
}
