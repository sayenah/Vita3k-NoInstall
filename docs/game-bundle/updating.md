# Updating Vita3K-NoInstall to a newer upstream Vita3K

This fork is a compact patch series (~13 commits: the no-install mount +
archive decrypt, the ROMs/DLC/Updates/License folders, the library UI, CI, and
tooling) on top of upstream `Vita3K/Vita3K`. To move it onto a newer upstream,
you **rebase**: replay our commits on top of the newer code, resolve any
collisions, push, and let CI build fresh installers/APKs.

Almost all of our code lives in **new files** that upstream never touches
(`io/bundle.*`, `packages/archive_*`, `app/roms_list.cpp`, `external/lzma-sdk/`,
these docs). Only the handful of upstream files we modified can conflict.

## One-time setup

```bash
cd ~/projects/vita3k-bundle
git remote add upstream https://github.com/Vita3K/Vita3K.git   # if not already present
```

## The update (one command)

```bash
tools/update-from-upstream.sh              # onto the latest upstream master
# or, recommended, onto a stable release tag:
tools/update-from-upstream.sh v0.2.2
```

The script fetches upstream, saves a `backup/pre-update-…` snapshot of your
branch, rebases, and resyncs submodules. If it hits a conflict it stops and
prints the exact commands to finish (resolve → `git add` → `git rebase --continue`).
To undo everything: `git branch -f feature/game-bundle backup/pre-update-…`.

## Build + collect the installers

```bash
git push --force-with-lease origin feature/game-bundle   # triggers Build CI
tools/fetch-latest-build.sh                              # waits for green, downloads
```

Artifacts land in `…/Downloads/Vita3k-NoInstall/<platform>/` — the Android APK
at `…/android/app.apk`, Windows under `…/windows-x64/`. Pass platform names to
fetch others, e.g. `tools/fetch-latest-build.sh android windows-x64 macos-arm64`.

## Where conflicts can happen

Only the upstream-owned files we edited. If any of these changed upstream, the
rebase will pause on it:

- `vita3k/io/src/{io,state_functions}.cpp`, `vita3k/io/include/io/{io,state,vfs}.h`, `vita3k/io/CMakeLists.txt`
- `vita3k/modules/module_parent.cpp`, `SceAppMgr`, `SceAppUtil`, `SceAvPlayer`
- `vita3k/packages/src/pkg.cpp`, `vita3k/packages/include/packages/pkg.h`, `vita3k/packages/CMakeLists.txt`
- `vita3k/app/src/{app,apps_list}.cpp`, `vita3k/app/include/app/{functions,state}.h`, `vita3k/app/CMakeLists.txt`
- `vita3k/gui-qt/src/{apps_list,main_window}.cpp`
- `vita3k/config/*`, `vita3k/main.cpp`, `vita3k/interface.cpp`
- `vita3k/android/jni/*`, `android/app/src/main/java/org/vita3k/emulator/**`

A conflict just means "upstream and we both edited these lines" — keep both
intentions (usually: keep upstream's change **and** re-apply our added
lines/branches), `git add`, continue.
