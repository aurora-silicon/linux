# Aurora kernel releases

This repository has one long-lived branch, `main`. Changes land through pull
requests. [Linux PR 3064](https://github.com/aurora-silicon/linux/pull/3064)
introduces the release workflow and shared kernel configuration. Each channel
becomes available after its first successful publication.

The organisation-wide rules (branch names, who may tag, the merge gate) are in
[branching-and-releases](https://github.com/aurora-silicon/agent-guidelines/blob/main/policy/branching-and-releases.md);
this file adds the kernel build details.

## Channels

| Channel | Where | Built when | Changes? |
| --- | --- | --- | --- |
| stable | the GitHub Release for a `v*` tag, e.g. [`releases/latest`](https://github.com/aurora-silicon/linux/releases/latest) | a `v*` tag is pushed on a `main` commit | never: a stable release is not rebuilt or edited |
| nightly | prerelease tag [`aurora-nightly`](https://github.com/aurora-silicon/linux/releases/tag/aurora-nightly) | once a day, if `main` moved since the last nightly | tag and assets replaced each night |
| edge | prerelease tag [`aurora-edge`](https://github.com/aurora-silicon/linux/releases/tag/aurora-edge) | every push to `main`; a running build always finishes and only the newest waiting push is built next | tag and assets replaced on every build |

Each release body names its exact source commit, and every asset carries a
build provenance attestation from `kernel-release.yml`
(`gh attestation verify <file> --repo aurora-silicon/linux`). `edge` is
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
   release body), once it has passed the
   [lab regression gate](https://github.com/aurora-silicon/agent-guidelines/blob/main/policy/branching-and-releases.md#4-cutting-a-stable-release):
   it boots and passes on every gate Mac with no new failures compared with the
   previous stable release. The gate looks for regressions only; missing
   features do not block a tag.
2. An Owner tags it and pushes the tag (only Owners create `v*` tags, and
   nobody moves or deletes one):

   ```sh
   git tag -a v0.2.0 -m 'Aurora v0.2.0' <sha>
   git push origin v0.2.0
   ```

3. The workflow builds it and publishes the release with generated notes.

Versions are `X.Y.Z`: Y goes up for a regular release, Z for a hotfix, and a
new upstream base is at least a minor bump.

Unfinished work remains in its pull request for a later release. The workflow
accepts stable tags on `main` and the hotfix branches described below. Stable
releases must remain immutable.

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
