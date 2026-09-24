#!/usr/bin/env bash
#
# Bring new Vita3K-Plus enhancements into Vita3K+ NoInstall.
#
# Usage:
#   tools/import-vita3k-plus.sh [plus-ref]   import up to plus-ref (default: all-enhancements)
#   tools/import-vita3k-plus.sh --finish     after resolving a conflict (see below)
#   tools/import-vita3k-plus.sh --abort      give up; the branch goes back to where it was
#   tools/import-vita3k-plus.sh --ci ...     for update-from-upstream.yml: a conflict or rewritten
#                                            Plus history is undone and reported via $GITHUB_OUTPUT
#
# .github/vita3k-plus.ref holds the Vita3K-Plus commit this fork last imported. The script
# cherry-picks the commits Plus made since then -- skipping merges and anything already in upstream
# Vita3K, which the fork gets by rebasing -- and squashes them into one "Import Vita3K-Plus" commit
# that also advances the ref file. One commit per import keeps the patch series that the upstream
# rebase replays short. Plus's own CI (.github/) is never imported: the fork owns its workflows.
#
# On a conflict (local runs) it stops mid cherry-pick. Fix the files, `git add` them,
# `git cherry-pick --continue` (repeat if it stops again), then `tools/import-vita3k-plus.sh --finish`.
#
# Exit codes: 0 imported or already up to date (and, with --ci, conflicts/rewrites that were
# reported), 1 stopped on a conflict, 2 anything else.

PLUS_URL=https://github.com/nckstwrt/Vita3K-Plus.git
UPSTREAM_URL=https://github.com/Vita3K/Vita3K.git
REF_FILE=.github/vita3k-plus.ref

cd "$(git rev-parse --show-toplevel)" || exit 2
STATE_FILE=$(git rev-parse --git-path vita3k-plus-import)

CI=0
MODE=start
PLUS_REF=all-enhancements
for arg in "$@"; do
  case "$arg" in
    --ci) CI=1 ;;
    --finish) MODE=finish ;;
    --abort) MODE=abort ;;
    -*) echo "!! Unknown option $arg" >&2; exit 2 ;;
    *) PLUS_REF=$arg ;;
  esac
done

output() {
  [ -n "${GITHUB_OUTPUT:-}" ] && printf '%s\n' "$@" >> "$GITHUB_OUTPUT"
  return 0
}

ensure_remote() {
  git remote get-url "$1" >/dev/null 2>&1 || git remote add "$1" "$2"
}

# The Plus-authored commits in OLD..NEW that the fork does not already have from upstream.
plus_commits() {
  git rev-list --reverse --no-merges "$2" ^"$1" ^upstream/master
}

finish() {
  local start new old commits count authors msg
  read -r start new < "$STATE_FILE" || { echo "!! No import in progress." >&2; exit 2; }
  if git rev-parse -q --verify CHERRY_PICK_HEAD >/dev/null; then
    echo "!! A cherry-pick is still in progress. Resolve it and run: git cherry-pick --continue" >&2
    exit 1
  fi
  old=$(cat "$REF_FILE")
  commits=$(plus_commits "$old" "$new")
  count=$(printf '%s' "$commits" | grep -c . || true)

  git reset -q --soft "$start"
  git restore --source="$start" --staged --worktree -- .github
  echo "$new" > "$REF_FILE"
  git add -- "$REF_FILE"

  msg="Import Vita3K-Plus enhancements up to ${new:0:8}

$count commit(s) from nckstwrt/Vita3K-Plus (${old:0:8}..${new:0:8}):"
  if [ "$count" -gt 0 ]; then
    msg+=$'\n'$(git log --no-walk=unsorted --format='- %h %s' $commits)
    authors=$(git log --no-walk=unsorted --format='Co-authored-by: %an <%ae>' $commits | sort -u)
    msg+=$'\n\n'"$authors"
  fi
  git commit -q -F - <<<"$msg" || exit 2
  rm -f "$STATE_FILE"
  echo "==> Imported $count Vita3K-Plus commit(s) as $(git rev-parse --short HEAD)."
  output "plus_status=ok" "plus_count=$count"
}

abort() {
  local start new
  read -r start new < "$STATE_FILE" || { echo "!! No import in progress." >&2; exit 2; }
  git cherry-pick --abort 2>/dev/null
  git reset -q --hard "$start"
  rm -f "$STATE_FILE"
  echo "==> Import abandoned; branch is back at ${start:0:8}."
}

start() {
  local old new commits conflicted stopped
  if [ -e "$STATE_FILE" ]; then
    echo "!! An import is already in progress: finish it with --finish or undo it with --abort." >&2
    exit 2
  fi
  if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    echo "!! Working tree is dirty. Commit or stash your changes first." >&2
    exit 2
  fi

  ensure_remote plus "$PLUS_URL"
  ensure_remote upstream "$UPSTREAM_URL"
  git fetch -q upstream master || exit 2
  git fetch -q plus "$PLUS_REF" || exit 2
  new=$(git rev-parse FETCH_HEAD)
  old=$(cat "$REF_FILE") || exit 2
  output "plus_old=$old" "plus_new=$new"

  if [ "$old" = "$new" ]; then
    echo "==> Already have Vita3K-Plus ${new:0:8}."
    output "plus_status=uptodate"
    return 0
  fi
  if ! git merge-base --is-ancestor "$old" "$new"; then
    echo "!! Vita3K-Plus ${new:0:8} does not contain the last import ${old:0:8}: its history was rewritten." >&2
    echo "   Import the new commits by hand, then set $REF_FILE to ${new}." >&2
    output "plus_status=rewritten"
    [ "$CI" = 1 ] && return 0
    exit 2
  fi

  commits=$(plus_commits "$old" "$new")
  echo "$(git rev-parse HEAD) $new" > "$STATE_FILE" || exit 2
  # Picks that turn out empty (already present) are kept and vanish in the squash.
  echo "==> Cherry-picking $(printf '%s' "$commits" | grep -c .) Vita3K-Plus commit(s) ${old:0:8}..${new:0:8}"
  if [ -z "$commits" ] || git cherry-pick --allow-empty --keep-redundant-commits $commits; then
    finish
    return 0
  fi

  conflicted=$(git diff --name-only --diff-filter=U)
  stopped=$(git log -1 --format='%h %s' CHERRY_PICK_HEAD 2>/dev/null)
  output "conflicts<<EOF" "Stopped at Vita3K-Plus commit: $stopped" $conflicted "EOF"
  output "plus_status=conflict"
  if [ "$CI" = 1 ]; then
    abort
    return 0
  fi
  cat <<EOF

  !!  Stopped on Vita3K-Plus commit $stopped -- it conflicts with this fork.
      Resolve it:
        1) edit each file below, fix the  <<<<<<<  =======  >>>>>>>  markers
$(printf '             %s\n' $conflicted)
        2) git add <file>
        3) git cherry-pick --continue      # repeat 1-3 if it stops again
        4) tools/import-vita3k-plus.sh --finish

      Or give up (branch left untouched):  tools/import-vita3k-plus.sh --abort
EOF
  exit 1
}

"$MODE"
