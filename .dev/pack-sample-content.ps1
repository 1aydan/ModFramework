# Copyright (c) 2026. Licensed for use in your own projects.
<#
.SYNOPSIS
	Packs the sample game's Content/ folder into a release archive.

.DESCRIPTION
	Templates/ModFrameworkSample/Content/ is gitignored — it is ~135 MB of stock UE5 Third Person
	template assets that this project did not author, and .uasset files do not delta-compress, so
	committing them would make every clone pay forever.

	Instead it ships as a GitHub release asset. Run this, then attach the output to a release.

	The archive contains a top-level Content/ folder, so extracting it inside
	Templates/ModFrameworkSample/ lands in the right place.

.PARAMETER OutputDirectory
	Where to write the archive. Defaults to <repo>/Dist (gitignored).

.EXAMPLE
	./.dev/pack-sample-content.ps1
	gh release create v0.1.0 Dist/ModFrameworkSample-Content.zip --notes "Sample content for UE 5.8"
#>
[CmdletBinding()]
param(
	[string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'

$RepoRoot   = Split-Path -Parent $PSScriptRoot
$SampleRoot = Join-Path $RepoRoot 'Templates/ModFrameworkSample'
$ContentDir = Join-Path $SampleRoot 'Content'

if (-not $OutputDirectory) { $OutputDirectory = Join-Path $RepoRoot 'Dist' }

if (-not (Test-Path $ContentDir))
{
	throw "No content at '$ContentDir'. Nothing to pack — see Templates/ModFrameworkSample/README.md."
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$Archive = Join-Path $OutputDirectory 'ModFrameworkSample-Content.zip'

if (Test-Path $Archive) { Remove-Item $Archive -Force }

$Stats = Get-ChildItem $ContentDir -Recurse -File | Measure-Object -Property Length -Sum
Write-Host ("Packing {0} files, {1:N1} MB ..." -f $Stats.Count, ($Stats.Sum / 1MB))

# Compress from the sample root so the archive carries a top-level Content/ folder.
Compress-Archive -Path $ContentDir -DestinationPath $Archive -CompressionLevel Optimal

$Size = (Get-Item $Archive).Length
Write-Host ("Wrote {0} ({1:N1} MB)" -f $Archive, ($Size / 1MB)) -ForegroundColor Green

if ($Size -gt 2GB)
{
	Write-Warning 'Archive exceeds 2 GB — GitHub release assets are capped at 2 GB per file.'
}

Write-Host ''
Write-Host 'Attach it to a release, and state the engine version in the notes:'
Write-Host "  gh release create <tag> `"$Archive`" --notes `"Sample content for UE 5.8`""
