#!/usr/bin/env bash
# Usage: ./scripts/push_to_github.sh https://github.com/<you>/draupnir.git [branch]
set -euo pipefail
REMOTE="${1:?pass the remote URL}"
BRANCH="${2:-main}"
git branch -M "$BRANCH"
git remote remove origin 2>/dev/null || true
git remote add origin "$REMOTE"
git push -u origin "$BRANCH"
echo "Pushed to $REMOTE ($BRANCH)."
