# Copyright (c) 2026. Licensed for use in your own projects.
<#
.SYNOPSIS
	Builds the sample game with both plugins installed.

.DESCRIPTION
	This is the integration check that .dev/build-harness.ps1 cannot make: the harness proves the
	plugins compile in isolation, this proves they compile as part of a real game that also depends
	on them from its own module.

	CLOSE THE EDITOR FIRST. A running editor holds locks on the module DLLs and the build will fail
	partway through, which also takes down the MCP server if you were using it.

.PARAMETER Clean
	Delete the plugins' Intermediate folders first.
#>
[CmdletBinding()]
param(
	[switch]$Clean
)

$Sample  = "F:\SelfProjects\Unreal\Plugins\ModFramework\Templates\ModFrameworkSample"
$Project = Join-Path $Sample "ModFrameworkSample.uproject"
$Build   = "F:\SelfProjects\Unreal\UE_5.8\Engine\Build\BatchFiles\Build.bat"
$Log     = Join-Path $Sample "build.log"

if (Get-Process -Name "UnrealEditor" -ErrorAction SilentlyContinue)
{
	Write-Warning "UnrealEditor is running. It holds module DLL locks and this build will likely fail."
}

if ($Clean)
{
	foreach ($p in @("ModFramework", "GameModSDK"))
	{
		$i = Join-Path $Sample "Plugins\$p\Intermediate"
		if (Test-Path $i) { Remove-Item -Recurse -Force $i; Write-Host "cleaned $p" }
	}
}

& $Build ModFrameworkSampleEditor Win64 Development -project="$Project" -waitmutex -NoHotReload 2>&1 |
	Tee-Object -FilePath $Log | Out-Null
$Code = $LASTEXITCODE

$Lines = Get-Content $Log
$Diagnostics = $Lines | Select-String -Pattern 'error [A-Z]+\d+|error LNK|: error:|Error: |fatal error' |
	ForEach-Object { $_.Line.Trim() } | Select-Object -Unique

Write-Host "=== exit $Code ==="
if ($Diagnostics)
{
	Write-Host "=== $($Diagnostics.Count) diagnostics ==="
	$Diagnostics | Select-Object -First 80
}
elseif ($Code -ne 0)
{
	Write-Host "=== no compiler diagnostics matched; last 25 log lines ==="
	$Lines | Select-Object -Last 25 | ForEach-Object { $_.TrimEnd() }
}
else
{
	Write-Host "SAMPLE BUILD SUCCEEDED"
}
