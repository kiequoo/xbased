param(
    [string]$Build = (Join-Path $PSScriptRoot "..\..\..\build\win"),
    [string]$Out = (Join-Path $PSScriptRoot "..\..\..\dist"),
    [string]$Version = "1.0.29"
)
# xbased's Windows release: the executable, its fonts and mods, the docs
# and licenses, in dist\xbased-<version>-windows-x64 and its .zip.
$ErrorActionPreference = "Stop"
$repo = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$name = "xbased-$Version-windows-x64"
$dst = Join-Path $Out $name
if (Test-Path $dst) { Remove-Item -Recurse -Force $dst }
New-Item -ItemType Directory -Force $dst | Out-Null
Copy-Item (Join-Path $Build "xbased.exe") $dst
Copy-Item -Recurse (Join-Path $repo "native\app\ui\fonts") (Join-Path $dst "fonts")
Copy-Item -Recurse (Join-Path $repo "mods") (Join-Path $dst "mods")
Copy-Item -Recurse (Join-Path $repo "cheats") (Join-Path $dst "cheats")   # the Xbox scene's cheat files, by title ID
Move-Item (Join-Path $dst "mods\README.md") (Join-Path $dst "MODS.md") -Force   # how to add patches
$readme = Join-Path $repo "docs\xbased\README.md"
if (-not (Test-Path $readme)) { $readme = Join-Path $repo "README.md" }
Copy-Item $readme (Join-Path $dst "README.md")
Copy-Item (Join-Path $repo "COPYING") $dst
Copy-Item (Join-Path $repo "LICENSE") $dst
Copy-Item -Recurse (Join-Path $repo "licenses") (Join-Path $dst "licenses")
# the executable keeps its settings and data beside it
"xbased keeps its settings, saves and states in this folder." | Set-Content -Encoding ascii (Join-Path $dst "portable.txt")
$zip = Join-Path $Out "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $dst "*") -DestinationPath $zip
Get-Item $zip | Select-Object Name, @{n = "MB"; e = { [math]::Round($_.Length / 1MB, 1) } }
