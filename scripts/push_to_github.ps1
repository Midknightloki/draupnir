# Usage:  .\scripts\push_to_github.ps1 -Remote "https://github.com/<you>/draupnir.git"
# Run from the repo root. Creates 'main', adds the remote, and pushes.
param(
  [Parameter(Mandatory=$true)][string]$Remote,
  [string]$Branch = "main"
)
git branch -M $Branch
git remote remove origin 2>$null
git remote add origin $Remote
git push -u origin $Branch
Write-Host "Pushed to $Remote ($Branch)."
