# SPDX-License-Identifier: GPL-3.0-or-later
# Generate the WinSparkle appcast.xml (repo root) for a release. The installer
# must already be signed with the EdDSA private key (see docs/RELEASING.md); pass
# the resulting sparkle:edSignature here. Computes the enclosure length + pubDate.
#
#   pwsh scripts\make-appcast.ps1 -Version 0.1.0.43 `
#        -SetupExe build\installer\SQLTerminal-0.1.0-setup.exe -Signature <base64>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Version,    # marketing.build, e.g. 0.1.0.43 (matches SQLT_VERSION_FULL)
    [Parameter(Mandatory)][string]$SetupExe,   # path to the built (and signed) installer
    [Parameter(Mandatory)][string]$Signature,  # sparkle:edSignature from sign_update
    [string]$Repo = 'arcanii/SQLTerminal-Win32',
    [string]$Tag,                              # GitHub release tag; defaults to v$Version
    [string]$Notes                             # release-notes URL; defaults to the release page
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $SetupExe)) { throw "Setup exe not found: $SetupExe" }
if (-not $Tag) { $Tag = "v$Version" }
$len = (Get-Item -LiteralPath $SetupExe).Length
$file = Split-Path -Path $SetupExe -Leaf
$url = "https://github.com/$Repo/releases/download/$Tag/$file"
if (-not $Notes) { $Notes = "https://github.com/$Repo/releases/tag/$Tag" }
$pub = (Get-Date).ToUniversalTime().ToString(
    'ddd, dd MMM yyyy HH:mm:ss +0000', [System.Globalization.CultureInfo]::InvariantCulture)
$out = Join-Path (Split-Path -Path $PSScriptRoot -Parent) 'appcast.xml'
$xml = @"
<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
  <channel>
    <title>SQLTerminal (Win32) Updates</title>
    <link>https://raw.githubusercontent.com/$Repo/main/appcast.xml</link>
    <description>Most recent updates to SQLTerminal for Windows</description>
    <language>en</language>
    <item>
      <title>Version $Version</title>
      <sparkle:releaseNotesLink>$Notes</sparkle:releaseNotesLink>
      <pubDate>$pub</pubDate>
      <enclosure url="$url"
                 sparkle:version="$Version"
                 sparkle:edSignature="$Signature"
                 length="$len"
                 type="application/octet-stream"/>
    </item>
  </channel>
</rss>
"@
Set-Content -LiteralPath $out -Value $xml -Encoding UTF8
Write-Host "Wrote $out"
Write-Host "  version=$Version  length=$len  url=$url"
Write-Host "Next: commit appcast.xml on main, and upload $file to the '$Tag' GitHub release."
