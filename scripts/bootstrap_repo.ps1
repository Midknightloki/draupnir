# Draupnir repo bootstrap (Windows / PowerShell)
# Run from the repo root (the 'draupnir' folder):
#   .\scripts\bootstrap_repo.ps1                                   # init + commit locally
#   .\scripts\bootstrap_repo.ps1 -Remote "https://github.com/<you>/draupnir.git"   # + push
param(
  [string]$Remote = "",
  [string]$Branch = "main"
)
# Remove any stray/broken .git from the scratchpad copy, then start clean.
if (Test-Path ".git") { Remove-Item -Recurse -Force ".git" }
git init | Out-Null
git add -A
git commit -m "Draupnir P1 scaffold: spec, M0 bring-up, docs, context, scripts"
git branch -M $Branch
if ($Remote -ne "") {
  git remote add origin $Remote
  git push -u origin $Branch
  Write-Host "Committed and pushed to $Remote ($Branch)."
} else {
  Write-Host "Committed locally on '$Branch'. Re-run with -Remote <url> to push."
}
