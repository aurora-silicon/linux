# Aurora kernel releases

This repository has one long-lived branch, `main`. Every change lands there
through a pull request, and every release is built from a commit on `main` by
[`kernel-release.yml`](.github/workflows/kernel-release.yml).

## Channels

| Channel | Where | Built when | Changes? |
| --- | --- | --- | --- |
| stable | the GitHub Release for a `v*` tag, e.g. [`releases/latest`](https://github.com/aurora-silicon/linux/releases/latest) | a `v*` tag is pushed on a `main` commit | never: a stable release is not rebuilt or edited |
| nightly | prerelease tag [`aurora-nightly`](https://github.com/aurora-silicon/linux/releases/tag/aurora-nightly) | once a day, if `main` moved since the last nightly | tag and assets replaced each night |
| edge | prerelease tag [`aurora-edge`](https://github.com/aurora-silicon/linux/releases/tag/aurora-edge) | every push to `main` (a newer push cancels the running build) | tag and assets replaced on every push |

Each release body names the exact `main` commit it was built from. `edge` is
for testing what just merged; `nightly` is the candidate pool for the next
stable tag; `stable` is what distributions ship by default.

## What a release contains

For kernel release `<krel>` (also in the `kernel.release` asset):

- `Image-<krel>.gz`: the arm64 kernel image
- `modules-<krel>.tar.zst`: the `lib/modules/<krel>` tree, modules signed
- `dtbs-<krel>.tar.zst`: all device trees built by the configuration
- `config-<krel>`, `System.map-<krel>`
- `module-signing-cert-<krel>.der`: the certificate the modules are signed with
- `linux-aurora*-bin-*.pkg.tar.zst`: Arch Linux packages from `make pacman-pkg`
- `source.commit`, `SHA256SUMS`

The configuration is the fleet configuration: `make aurora_defconfig asahi.config`
(`arch/arm64/configs/aurora_defconfig` plus `asahi.config`), built with the
toolchain versions pinned in the workflow.

Each build signs its modules with a key generated for that build and deletes
the private key afterwards. Our lab fleet kits are signed separately and are
not published here.

## Cutting a stable release

Stable releases are time-boxed. On the release cadence:

1. Take the commit the current `aurora-nightly` was built from (it is in the
   release body), once it has passed the lab regression gate: every lab Mac
   (M1 Pro, M2 Air, M2 mini, M3 Air, M4 Air, M4 Pro, Neo) boots it with no new
   failures compared with the previous stable release. The gate looks for
   regressions only; missing features do not block a tag.
2. Tag it and push the tag:

   ```sh
   git tag -a v0.2.0 -m 'Aurora v0.2.0' <sha>
   git push origin v0.2.0
   ```

3. The workflow builds it and publishes the release with generated notes.

Never hold a tag for unfinished work. Work that is not ready stays in its pull
request and goes out in the next release. The workflow refuses a tag that is
not on `main` and never overwrites a stable release that already exists.

## Hotfixes

Only a boot failure, data loss, a security problem or a risk of hardware damage
in a stable release justifies a hotfix. Everything else is fixed forward on
`main` and ships in the next release.

1. Branch `hotfix/v<X.Y.Z>` from the stable tag, fix it there and tag
   `v<X.Y.Z+1>` on that branch. The tag builds and publishes like any stable
   release.
2. Forward-port the same commits to `main` through a pull request; each
   forward-ported commit carries a `Forward-port: <sha>` trailer naming the
   hotfix commit.
3. Delete the hotfix branch once the forward-port is merged.

Hotfix tags are the one case where a stable tag is not on `main`; the
workflow accepts them only from a `hotfix/` branch.

## Upstream stable updates

Upstream stable point releases for the current base (for example 7.3.y) are
merged into `main` weekly through a pull request, and at once when they carry
security fixes.

## Kernel version upgrades

Moving to a new upstream version (for example 7.4) happens on one branch,
`rebase/v7.4`, opened as a pull request into `main`. It is built and
fleet-tested like any other change and merged when it passes. `main` is never
rebased in place, and nobody stacks other work on the rebase branch; only fixes
needed for the rebase itself go there.
