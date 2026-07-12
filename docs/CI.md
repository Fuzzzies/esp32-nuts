# CI, releases, and Actions storage

This repo keeps GitHub Actions **artifact storage** low. Artifacts are staging
bins for releases — not a download channel for every push.

Workflows: [`.github/workflows/build.yml`](../.github/workflows/build.yml),
[`.github/workflows/pages.yml`](../.github/workflows/pages.yml).

## When CI runs

| Trigger | `build.yml` | Artifacts uploaded? |
|---------|-------------|---------------------|
| Pull request | All jobs build/test | **No** |
| Push to `main` | All jobs build/test | **No** |
| Push `v*` tag | Build + release | **Yes** (1-day retention) |
| `workflow_dispatch` | All jobs build/test | **No** (unless you tag) |

Feature-branch pushes do **not** trigger `build.yml` (only `main`, tags, and PRs).

`concurrency` cancels superseded runs on the same ref so rapid pushes do not stack
full ESP-IDF + Arduino builds.

## Jobs

| Job | Purpose | Artifact |
|-----|---------|----------|
| **test-standalone** | Validate `esp32-flasher.json`, compile bootstrap, check partition table | `esp32-nuts-bootstrap-<sha>.factory.bin` on tags only |
| **usb** | ESP-IDF build + merge-bin | `esp32-nuts-usb-<sha>.bin` on tags only |
| **emulator** | Compile dev emulator sketch | *(none — compile-only)* |
| **release** | Attach bins to GitHub Release | Runs on `v*` tags only |
| **pages** | Deploy [web flasher](flasher/) to GitHub Pages | On `release: published` |

Emulator firmware is **dev-only** and is not published to Releases or Pages.

## Getting firmware binaries

| Need | How |
|------|-----|
| **Released builds** | Push a `v*` tag (e.g. `v0.1.0`) → GitHub Release gets bootstrap + USB `.bin` files |
| **Web flash** | After a release, GitHub Pages hosts [docs/flasher/](flasher/) (enable Pages → GitHub Actions in repo settings) |
| **Local build** | ESP-IDF: `idf.py build merge-bin` · Bootstrap: see [BOOTSTRAP.md](BOOTSTRAP.md) |
| **CI artifact from a PR** | Not stored — build locally or wait for a tag |

Release job needs artifacts from **test-standalone** and **usb** on the same tag
push. Do not upload emulator to releases.

## Storage policy (why it is this way)

GitHub Actions artifact storage is limited on free/low tiers (~0.5 GB/month
included). This repo previously uploaded **three** artifacts on every push and
PR (~6 MB/run, 90-day default retention), which adds up quickly during active
development.

**Current policy (since PR #3):**

- Upload artifacts **only** on `v*` tags
- `retention-days: 1` — release job downloads them in the same workflow
- No emulator artifacts
- Push trigger limited to `main` (not `**`)

**One-time cleanup (2026-07):** 29 stale artifacts (~18.6 MB) were deleted.
Releases were already empty.

## Publishing a release

```sh
git checkout main
git pull
# optional: verify CI is green on main
git tag v0.1.0
git push origin v0.1.0
```

That triggers:

1. `build.yml` — bootstrap + USB artifacts (1-day retention)
2. `release` job — attaches `*.bin` to GitHub Release
3. `pages.yml` — deploys web flasher with release binaries

No `v*` tag → no Release, no Pages firmware bundle.

## Auditing / cleaning storage

Replace `Fuzzzies/esp32-nuts` if checking another repo.

```sh
# Artifact summary
gh api "repos/Fuzzzies/esp32-nuts/actions/artifacts?per_page=100" \
  --jq '{count: (.artifacts|length), total_mb: ([.artifacts[].size_in_bytes]|add/1048576|floor), unexpired: ([.artifacts[]|select(.expired==false)]|length)}'

# Releases
gh release list --limit 20
```

**Delete all stored Actions artifacts** (does not remove GitHub Release files):

```powershell
$ids = gh api --paginate "repos/Fuzzzies/esp32-nuts/actions/artifacts" `
  --jq '.artifacts[]|select(.expired==false)|.id'
foreach ($id in $ids) {
  gh api --method DELETE "repos/Fuzzzies/esp32-nuts/actions/artifacts/$id"
}
```

Only delete old **releases** after confirming nothing depends on them:

```sh
gh release delete v0.0.XX --yes   # keep latest unless intentional
```

## Changing this policy

If you add `upload-artifact` on every push again, set a short `retention-days`
and document why (e.g. nightly builds users download from Actions). Prefer tag-only
uploads for firmware this size.

See also [CONTRIBUTING.md](../CONTRIBUTING.md) for partition/flasher maintenance.
