# Larzscript one-line installer for Windows:
#   irm https://raw.githubusercontent.com/larz-scripter/larzscript/main/install.ps1 | iex
# Downloads the latest native larzscript.exe + the larzpkg package manager,
# and adds it to your PATH so `larzscript` works in any new cmd/PowerShell
# window - the same experience as installing Python.
$ErrorActionPreference = "Stop"

$binDir = Join-Path $env:LOCALAPPDATA "Larzscript\bin"
$libDir = Join-Path $env:USERPROFILE ".larzscript\lib"
New-Item -ItemType Directory -Force -Path $binDir, $libDir | Out-Null

Write-Host "downloading larzscript (windows-x86_64) ..."
Invoke-WebRequest -UseBasicParsing `
  -Uri "https://github.com/larz-scripter/larzscript/releases/latest/download/larzscript-windows-x86_64.exe" `
  -OutFile (Join-Path $binDir "larzscript.exe")

Invoke-WebRequest -UseBasicParsing `
  -Uri "https://raw.githubusercontent.com/larz-scripter/larzscript/main/tools/larzpkg.lz" `
  -OutFile (Join-Path (Join-Path $env:USERPROFILE ".larzscript") "larzpkg.lz")

$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
$already = ($userPath -split ";") -contains $binDir
if (-not $already) {
  $newPath = if ([string]::IsNullOrEmpty($userPath)) { $binDir } else { "$userPath;$binDir" }
  [Environment]::SetEnvironmentVariable("Path", $newPath, "User")
  $env:Path = "$env:Path;$binDir"   # so it also works in *this* session, not just new ones
}

Write-Host ""
Write-Host "installed: $binDir\larzscript.exe"
Write-Host (& "$binDir\larzscript.exe" --version)
if (-not $already) {
  Write-Host "added $binDir to your User PATH - open a NEW cmd/PowerShell window for it to take effect"
}
Write-Host ""
Write-Host "try it:"
Write-Host "  larzscript repl"
Write-Host "  larzscript $env:USERPROFILE\.larzscript\larzpkg.lz install mathx"
Write-Host '  larzscript -e "print(\"hello from larzscript\")"'
