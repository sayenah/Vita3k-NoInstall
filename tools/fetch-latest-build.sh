#!/usr/bin/env bash
#
# Download the CI build for the CURRENT commit to the Windows Downloads folder.
# Run this after `git push`, while (or after) CI runs -- it waits for each
# platform's job to go green, then downloads that artifact.
#
# Usage:
#   tools/fetch-latest-build.sh [platform ...]
#
#   platforms default to:  android windows-x64
#   valid values:          android windows-x64 windows-arm64
#                          linux-x64 linux-arm64 macos-arm64 macos-x64
#
# Results land in:  <Downloads>/Vita3k-NoInstall/<platform>/
#   e.g. the APK ends up at  .../Vita3k-NoInstall/android/app.apk

REPO=sayenah/Vita3k-NoInstall
DEST="/mnt/c/Users/sinmu/Downloads/Vita3k-NoInstall"
WF=c-cpp.yml

cd "$(git rev-parse --show-toplevel)" || exit 1
SHA=$(git rev-parse HEAD)
SHORT=$(git rev-parse --short=8 HEAD)
PLATFORMS=("$@")
[ ${#PLATFORMS[@]} -eq 0 ] && PLATFORMS=(android windows-x64)

echo "==> Finding the Build CI run for commit $SHORT ..."
RUN=""
for _ in $(seq 1 10); do
  # Prefer the push-event run (it checks out our exact commit); fall back to any run
  # for this sha (a pull_request run checks out a synthetic merge commit instead).
  RUN=$(gh run list -R "$REPO" --workflow="$WF" --limit 40 \
        --json databaseId,headSha,event \
        -q "[.[] | select(.headSha==\"$SHA\")] | (map(select(.event==\"push\")) + .) | .[0].databaseId")
  [ -n "$RUN" ] && break
  echo "   ...not registered yet, waiting 15s (did you push this commit?)"
  sleep 15
done
if [ -z "$RUN" ]; then
  echo "!! No Build CI run found for $SHORT. Push first:"
  echo "     git push --force-with-lease origin feature/game-bundle"
  exit 1
fi
echo "   run: https://github.com/$REPO/actions/runs/$RUN"

for plat in "${PLATFORMS[@]}"; do
  case "$plat" in
    android) job="Android";;
    *)       job="Desktop ($plat)";;
  esac
  echo "==> Waiting for job '$job' ..."
  while :; do
    st=$(gh run view "$RUN" -R "$REPO" --json jobs \
         -q ".jobs[] | select(.name==\"$job\") | \"\(.status)/\(.conclusion)\"")
    case "$st" in
      completed/success) echo "   $job: success"; break;;
      completed/*)       echo "   $job did NOT succeed ($st) -- skipping"; job=""; break;;
      "")                echo "   (no '$job' job in this run) -- skipping"; job=""; break;;
      *)                 sleep 30;;
    esac
  done
  [ -z "$job" ] && continue

  # Resolve the real artifact name by platform suffix -- the sha prefix in the name is the run's
  # checkout sha, which differs between push (our commit) and pull_request (a merge commit) runs.
  name=$(gh api "repos/$REPO/actions/runs/$RUN/artifacts" --paginate \
         -q ".artifacts[] | select(.name|endswith(\"-$plat\")) | .name" | head -1)
  if [ -z "$name" ]; then
    echo "   !! no artifact ending in '-$plat' on this run -- skipping"
    continue
  fi
  out="$DEST/$plat"
  echo "==> Downloading $name -> $out"
  rm -rf "$out"; mkdir -p "$out"
  if gh run download "$RUN" -R "$REPO" -n "$name" -D "$out"; then
    echo "   OK  $(find "$out" -type f | wc -l) file(s) in $out"
  else
    echo "   !! download failed for $name"
  fi
done
echo "Done. Builds are under $DEST"
