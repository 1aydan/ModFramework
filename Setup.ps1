# Copyright (c) 2026. Licensed for use in your own projects.
<#
.SYNOPSIS
	Links the two plugins into the sample game project so it can be opened, built and packaged.

.DESCRIPTION
	This repository holds two sibling plugins and one sample game that consumes them:

		ModFramework/                   the game-agnostic framework plugin
		GameModSDK/                     the reference public SDK plugin
		Templates/ModFrameworkSample/   the sample game that installs both

	Rather than committing a second copy of each plugin inside the sample project, this script
	creates directory junctions (Windows) or symbolic links (other platforms) at
	Templates/ModFrameworkSample/Plugins/. That folder is gitignored, so a fresh clone runs this
	script once and everything works.

	Junctions are used in preference to the project's "AdditionalPluginDirectories" setting on
	purpose. That setting is honoured only under WITH_EDITOR: a packaged build silently discards it
	and looks in <ProjectDir>/../RemappedPlugins/ instead. Since the whole point of this framework
	is loading mods into a *packaged* game, a mechanism that works in the editor and then quietly
	fails at packaging time is the wrong default.

.PARAMETER Remove
	Delete the links instead of creating them.

.EXAMPLE
	./Setup.ps1
	./Setup.ps1 -Remove
#>
[CmdletBinding()]
param(
	[switch]$Remove
)

$ErrorActionPreference = 'Stop'

$RepoRoot    = $PSScriptRoot
$SampleRoot  = Join-Path $RepoRoot 'Templates/ModFrameworkSample'
$PluginsDir  = Join-Path $SampleRoot 'Plugins'
$IsWindows_  = [System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
	[System.Runtime.InteropServices.OSPlatform]::Windows)

$Links = @(
	@{ Name = 'ModFramework'; Target = Join-Path $RepoRoot 'ModFramework' },
	@{ Name = 'GameModSDK';   Target = Join-Path $RepoRoot 'GameModSDK'   }
)

if (-not (Test-Path $SampleRoot))
{
	throw "Sample project not found at '$SampleRoot'. Run this script from the repository root."
}

if ($Remove)
{
	foreach ($Link in $Links)
	{
		$Path = Join-Path $PluginsDir $Link.Name
		if (Test-Path $Path)
		{
			# Remove-Item on a junction deletes the link, not the target — but only when the path
			# is not passed with -Recurse on older hosts, so delete via the directory info API.
			[System.IO.Directory]::Delete($Path, $false)
			Write-Host "  removed  $($Link.Name)"
		}
	}
	Write-Host 'Links removed.' -ForegroundColor Green
	return
}

New-Item -ItemType Directory -Force -Path $PluginsDir | Out-Null

foreach ($Link in $Links)
{
	$Name   = $Link.Name
	$Target = $Link.Target
	$Path   = Join-Path $PluginsDir $Name

	if (-not (Test-Path (Join-Path $Target "$Name.uplugin")))
	{
		throw "Expected '$Target/$Name.uplugin' but it does not exist. The repository layout is wrong."
	}

	if (Test-Path $Path)
	{
		$Existing = Get-Item $Path -Force
		if ($Existing.LinkType -and ($Existing.Target -contains $Target))
		{
			Write-Host "  ok       $Name"
			continue
		}
		if ($Existing.LinkType)
		{
			[System.IO.Directory]::Delete($Path, $false)
		}
		else
		{
			throw "'$Path' already exists and is a real directory, not a link. Delete it and re-run."
		}
	}

	$Type = if ($IsWindows_) { 'Junction' } else { 'SymbolicLink' }
	New-Item -ItemType $Type -Path $Path -Target $Target | Out-Null
	Write-Host "  linked   $Name  ->  $Target"
}

Write-Host ''
Write-Host 'Sample project linked. Next:' -ForegroundColor Green
Write-Host '  1. Right-click Templates/ModFrameworkSample/ModFrameworkSample.uproject'
Write-Host '     -> Generate Visual Studio project files'
Write-Host '  2. Open the .uproject (or build the ModFrameworkSampleEditor target).'
