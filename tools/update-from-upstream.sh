#!/usr/bin/env bash
#
# Carry the Vita3K-NoInstall patch series onto a newer upstream Vita3K.
#
# Usage:
#   tools/update-from-upstream.sh [upstream-ref]
#
#   upstream-ref  What to rebase onto. Default: upstream/master (bleeding edge).
#                 Pass a release tag for a stable target, e.g.
#                   tools/update-from-upstream.sh v0.2.2
#
# It fetches upstream, snapshots your branch to a backup/ branch, rebases the
# fork's commits on top, and (on success) resyncs submodules. On a conflict it
# stops and prints exactly what to do next. Your work is never lost: the backup
# branch always points at the pre-update state.

BRANCH=feature/game-bundle
TARGET="${1:-upstream/master}"

cd "$(git rev-parse --show-toplevel)" || exit 1

if ! git remote get-url upstream >/dev/null 2>&1; then
  echo "!! No 'upstream' remote. Add it once with:"
  echo "     git remote add upstream https://github.com/Vita3K/Vita3K.git"
  exit 1
fi

# A rebase on a dirty tree is unsafe — refuse.
if [ -n "$(git status --porcelain)" ]; then
  echo "!! Working tree is dirty. Commit or stash your changes first, then re-run."
  git status --short
  exit 1
fi

echo "==> Fetching upstream (branches + tags)..."
git fetch upstream --tags || exit 1

git checkout "$BRANCH" || exit 1

BACKUP="backup/pre-update-$(date +%Y%m%d-%H%M%S)"
git branch -f "$BACKUP" "$BRANCH"
echo "==> Snapshot saved: $BACKUP   (undo everything with:  git branch -f $BRANCH $BACKUP)"

echo "==> Rebasing $BRANCH onto $TARGET ..."
if git rebase "$TARGET"; then
  echo "==> Resyncing submodules (upstream often bumps them)..."
  git submodule update --init --recursive
  if [ ! -f vita3k/io/src/bundle.cpp ]; then
    echo "!! bundle.cpp is missing -- the rebase dropped a file. Restore: git branch -f $BRANCH $BACKUP"
    exit 1
  fi
  cat <<EOF

  OK  Rebased onto $TARGET, submodules synced, patch files intact.
      Next:
        git push --force-with-lease origin $BRANCH    # kick off CI
        tools/fetch-latest-build.sh                   # pull the new APK when green
EOF
else
  cat <<EOF

  !!  Rebase paused on a conflict -- normal when upstream changed a file you also edited.
      Resolve it:
        1) git status              # files under "both modified" are the conflicts
        2) edit each, fix the  <<<<<<<  =======  >>>>>>>  markers, save
        3) git add <file>
        4) git rebase --continue   # repeat 1-4 until it says the rebase is complete

      Then finish with:
        git submodule update --init --recursive
        git push --force-with-lease origin $BRANCH

      Or bail out completely (branch left untouched):  git rebase --abort
      Your pre-update snapshot is:  $BACKUP
EOF
  exit 1
fi
