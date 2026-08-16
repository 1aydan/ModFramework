# Copyright (c) 2026. Licensed for use in your own projects.
<#
.SYNOPSIS
	Proves the Game Mod SDK is usable without the game's source.

.DESCRIPTION
	Builds Templates/ModAuthorSample, a project that enables ModFramework and GameModSDK and has no
	reference whatsoever to the game module or the game project.

	This is the only automated evidence that the SDK boundary is real. Every other build in this
	repository has the game present, so a leak - an SDK header including a game header, an SDK type
	whose signature names a game type, a Build.cs dependency that crept in - compiles fine there and
	is invisible until a mod author reports it.

	If this fails, do NOT fix it by adding a dependency to ModAuthorSample. The failure is telling
	you the SDK needs something it should be exposing through a UModAPI instead.

.PARAMETER Clean
	Delete the plugins' Intermediate folders in the mod-author project first.
#>
[CmdletBinding()]
param(
	[switch]$Clean
)

$Project = "F:\SelfProjects\Unreal\Plugins\ModFramework\Templates\ModAuthorSample"
$UProject = Join-Path $Project "ModAuthorSample.uproject"
$Build = "F:\SelfProjects\Unreal\UE_5.8\Engine\Build\BatchFiles\Build.bat"
$Log = Join-Path $Project "build.log"

if (-not (Test-Path $UProject))
{
	throw "No mod-author project at '$UProject'. Run Setup.ps1 first."
}

if (Get-Process -Name "UnrealEditor" -ErrorAction SilentlyContinue)
{
	Write-Warning "UnrealEditor is running and may hold module DLL locks."
}

if ($Clean)
{
	foreach ($p in @("ModFramework", "GameModSDK"))
	{
		$i = Join-Path $Project "Plugins\$p\Intermediate"
		if (Test-Path $i) { Remove-Item -Recurse -Force $i; Write-Host "cleaned $p" }
	}
}

& $Build ModAuthorSampleEditor Win64 Development -project="$UProject" -waitmutex -NoHotReload 2>&1 |
	Tee-Object -FilePath $Log | Out-Null
$Code = $LASTEXITCODE

$Lines = Get-Content $Log
$Diagnostics = $Lines | Select-String -Pattern 'error [A-Z]+\d+|error LNK|: error:|Error: |fatal error' |
	ForEach-Object { $_.Line.Trim() } | Select-Object -Unique

Write-Host "=== exit $Code ==="
if ($Diagnostics)
{
	Write-Host "=== SDK BOUNDARY VIOLATED: $($Diagnostics.Count) diagnostics ===" -ForegroundColor Red
	$Diagnostics | Select-Object -First 60
	Write-Host ''
	Write-Host 'The SDK depends on something a mod author does not have. Expose it through a UModAPI'
	Write-Host 'rather than adding a dependency to ModAuthorSample.'
}
elseif ($Code -ne 0)
{
	Write-Host "=== no compiler diagnostics matched; last 25 log lines ==="
	$Lines | Select-Object -Last 25 | ForEach-Object { $_.TrimEnd() }
}
else
{
	Write-Host 'SDK BOUNDARY HOLDS: the SDK builds with no game source present.' -ForegroundColor Green
}
