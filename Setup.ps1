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
$IsWindows_  = [System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
	[System.Runtime.InteropServices.OSPlatform]::Windows)

$Links = @(
	@{ Name = 'ModFramework'; Target = Join-Path $RepoRoot 'ModFramework' },
	@{ Name = 'GameModSDK';   Target = Join-Path $RepoRoot 'GameModSDK'   }
)

# Both sample projects get the same two plugins - and that is the point. ModAuthorSample has no
# access to the game module or the game project, so if a mod builds there the SDK is genuinely
# shippable without game source.
$Projects = @(
	Join-Path $RepoRoot 'Templates/ModFrameworkSample'
	Join-Path $RepoRoot 'Templates/ModAuthorSample'
)

foreach ($ProjectRoot in $Projects)
{
	if (-not (Test-Path $ProjectRoot))
	{
		throw "Project not found at '$ProjectRoot'. Run this script from the repository root."
	}

	$ProjectName = Split-Path $ProjectRoot -Leaf
	$PluginsDir = Join-Path $ProjectRoot 'Plugins'
	Write-Host "$ProjectName" -ForegroundColor Cyan

	if ($Remove)
	{
		foreach ($Link in $Links)
		{
			$Path = Join-Path $PluginsDir $Link.Name
			if (Test-Path $Path)
			{
				# Deleting via the directory API removes the junction itself. Remove-Item -Recurse on
				# a junction can follow it and delete the TARGET's contents — which here would be the
				# plugin source.
				[System.IO.Directory]::Delete($Path, $false)
				Write-Host "  removed  $($Link.Name)"
			}
		}
		continue
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
		Write-Host "  linked   $Name"
	}
}

if ($Remove)
{
	Write-Host ''
	Write-Host 'Links removed.' -ForegroundColor Green
	return
}

Write-Host ''
Write-Host 'Both projects linked.' -ForegroundColor Green
Write-Host '  Templates/ModFrameworkSample  - the game: installs the framework, defines its modding surface'
Write-Host '  Templates/ModAuthorSample     - the mod author: has the SDK and NOTHING else'
Write-Host ''
Write-Host 'Right-click either .uproject -> Generate Visual Studio project files, then open it.'
