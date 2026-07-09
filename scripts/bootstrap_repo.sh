#!/usr/bin/env bash
# Draupnir repo bootstrap (macOS / Linux / Git Bash)
# Run from the repo root (the 'draupnir' folder):
#   ./scripts/bootstrap_repo.sh                                        # init + commit
#   ./scripts/bootstrap_repo.sh https://github.com/<you>/draupnir.git  # + push
set -euo pipefail
REMOTE="${1:-}"
BRANCH="${2:-main}"
rm -rf .git
git init -q
git add -A
git commit -qm "Draupnir P1 scaffold: spec, M0 bring-up, docs, context, scripts"
git branch -M "$BRANCH"
if [ -n "$REMOTE" ]; then
  git remote add origin "$REMOTE"
  git push -u origin "$BRANCH"
  echo "Committed and pushed to $REMOTE ($BRANCH)."
else
  echo "Committed locally on '$BRANCH'. Re-run with a remote URL to push."
fi
