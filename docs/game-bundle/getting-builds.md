# Getting a fresh Vita3K-NoInstall build (no coding needed)

This fork adds "play games with no install" + a ROMs-folder library on top of the
official Vita3K. When the official Vita3K team updates their emulator, you can pull
their update in and get a new Android + Windows build **entirely from the GitHub
website** — no computer setup, no commands.

There are two ways to get a build, and both end at the same place: the **Releases**
page, where you download the app.

---

## Option A — just wait (automatic)

Every **Monday**, GitHub automatically checks whether the official Vita3K changed.
If it did (and there's no conflict), it builds a new version and posts it to Releases.

So most of the time you just:

1. Go to **https://github.com/sayenah/Vita3k-NoInstall/releases/latest**
2. Download the file you need (see [Installing](#installing) below).

That's it.

---

## Option B — get a build right now (the button)

If you don't want to wait for Monday, or a new Vita3K just came out:

1. Open **https://github.com/sayenah/Vita3k-NoInstall/actions/workflows/update-from-upstream.yml**
   (You may need to be signed in to your GitHub account.)
2. Click the grey **"Run workflow"** button on the right.
3. Leave the box set to `master` and click the green **"Run workflow"**.
4. Wait about **30–45 minutes** (it's building the whole emulator for phone + PC).
5. Go to **https://github.com/sayenah/Vita3k-NoInstall/releases/latest** and download.

You can close the tab and come back later — it keeps running on GitHub's servers.

---

## Installing

On the Releases page you'll see up to two files:

- **`Vita3K-NoInstall.apk`** — for your **Android** phone/tablet.
  Download it on the device, open it, and tap **Install** (Android may ask you to
  "allow installing from this source" the first time — say yes). It installs over the
  old version; your settings and ROMs folder stay.

- **`Vita3K-NoInstall-windows-x64.zip`** — for **Windows**.
  Download, right-click → **Extract All**, then open the `bin` folder and run
  **`Vita3K.exe`**.

Your games/ROMs folder setting carries over between updates, so after installing you
can just open it and your list is there.

---

## If something goes wrong

- If the button run shows a **red ✗**, or you get an email about a new **"Issue"** on
  the repo titled *"Upstream update needs manual conflict resolution"* — that means the
  official Vita3K changed something that overlaps our fixes, and it needs a developer to
  sort out by hand. **Nothing is broken** and the last working build is still on the
  Releases page. Just let the developer know.
- If a Release doesn't appear after ~45 minutes, check the **Actions** tab for a red ✗
  and send a screenshot to the developer.

---

## For the developer (how it works / one-time setup)

Already set up on this repo: default branch is `feature/game-bundle`, Actions are
enabled, and the workflow token has write access (proven by the auto-created
`backup/auto-*` branch).

Workflows (all conflict-free, since they're new files upstream never touches):

- **`.github/workflows/update-from-upstream.yml`** — `workflow_dispatch` (the button) +
  a weekly `schedule`. Snapshots the branch to `backup/auto-<timestamp>`, rebases our
  commits onto the chosen upstream ref, force-pushes, then dispatches Build CI. On a
  rebase conflict it opens an issue and stops (no push). To turn off the weekly run,
  delete the `schedule:` block.
- **`.github/workflows/publish-release.yml`** — after Build CI succeeds on our branch,
  downloads the Android + Windows artifacts and (re)publishes them as the `latest`
  Release. Ignores pull-request builds.
- **`c-cpp.yml`** got a one-line `workflow_dispatch:` trigger so the update workflow can
  start it (a `GITHUB_TOKEN` push does not fire push-triggered workflows).

Local equivalents live in `tools/` (`update-from-upstream.sh`, `fetch-latest-build.sh`)
and the rebase reference is `docs/game-bundle/updating.md`. When the auto-rebase opens a
conflict issue, resolve it locally with `tools/update-from-upstream.sh <ref>` and push.

Housekeeping: `backup/auto-*` branches accumulate one per successful update. Delete old
ones anytime with `git push origin --delete backup/auto-<timestamp>`.
