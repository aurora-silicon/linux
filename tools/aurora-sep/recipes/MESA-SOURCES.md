# M3 Mesa: sources and recipes

Every Mesa package the installer ships to M3 Macs is built from a public branch of
[iconidentify/mesa-omacom](https://github.com/iconidentify/mesa-omacom). The exact commits
are recorded in each release's `mesa-matched-stack.json`.

| Package and prefix | Used by | Branch | Commit | Recipe |
|---|---|---|---|---|
| `mesa-m3`, `/opt/mesa-m3` | M3 Pro fallback, M3 Air legacy profile | `m3/legacy` | `7ff89a4da387` | `mesa-m3/` |
| `mesa-m3`, `/opt/mesa-m3/25g83` | M3 Air on 26.6.2, OpenGL only | `m3/air-hal200` | `78908f4d18af` | `mesa-m3/` |
| `mesa-m3-native-pro`, `/opt/mesa-m3/native-pro` | M3 Pro desktop OpenGL | `fix/native-index-offset-integrated-20261013` | `d616d0650d6f` | `release19/native-pro/` |
| `mesa-m3-pro-vulkan`, `/opt/mesa-m3/pro-vulkan` | M3 Pro Vulkan | `review/aurora-mesa-foundation-20261012` | `b0f56952b7f0` | `release19/pro-vulkan/` |
| `mesa-m3-g15c`, `/opt/mesa-m3-g15c` | M3 Max (T6031) test kit | `m3max/g15c` | `ff120e88cfc0` | `mesa-m3-g15c/` |

`mesa-m3` builds two trees into one package: the legacy tree and the HAL200 tree. The
`mesa-m3-g15c` branch is four commits on top of the legacy tree.

## Making the source tarballs

The recipes take `git archive` tarballs of those commits. For example:

```sh
git clone https://github.com/iconidentify/mesa-omacom.git
cd mesa-omacom
git archive --prefix=mesa-26.1.4/ 7ff89a4da387bd561b78efd5f09c98ca8ae18538 | zstd -19 -o ../mesa-m3-src-7ff89a4da38.tar.zst
git archive --prefix=mesa-hal200/ 78908f4d18af2274b5df08c4ad9d953c25980b53 | zstd -19 -o ../mesa-m3-hal200-78908f4d18af.tar.zst
git archive --prefix=mesa-26.1.4/ ff120e88cfc047b69ed412b2276599e3b2a426de | zstd -19 -o ../mesa-m3-g15c-src-ff120e88cfc.tar.zst
```

The prefix and file name must match the recipe's `source=` entry. A different zstd version
or level gives a different tarball checksum for the same tree. Either compare the extracted
tree with the commit, or run `updpkgsums` after checking it.

The other sources are the upstream release tarballs listed in each PKGBUILD, with their
sha256: `llvm-project` 22.1.8 (libclc), SPIRV-LLVM-Translator, SPIRV-Headers and SPIRV-Tools.

## Contributing

- Open a pull request against the matching branch in iconidentify/mesa-omacom.
- Commit messages and comments describe what the code does and how it was tested on
  hardware.
- A change reaches users only through a recipe here and a release that pins its commit.

The MacBook Neo Mesa (`mesa-neo`) is not published yet.
