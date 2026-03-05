# CI Packaging Plan — JarvisAgent

Last updated: 2026-03-04

This document breaks down the work required to add package builds and artifact
uploads to GitHub Actions CI, plus a Launchpad source package for Ubuntu/Debian.

---

## Current CI State

| Workflow | Runner | Build | Package | Artifact |
|----------|--------|-------|---------|----------|
| `linux-workflow.yml` | `ubuntu-latest` | Release + Debug | — | — |
| `windows-workflow.yml` | `windows-latest` | Release + Debug | ZIP | `JarvisAgent-Windows-zip` |
| `macos-workflow.yml` | `macos-latest` | Release + Debug | DMG | `JarvisAgent-macOS-dmg` |
| `docker-publish.yml` | `ubuntu-latest` | Docker image | OCI push | ghcr.io |

**Goal:** Every push produces downloadable artifacts for every supported format.

---

## Architecture

```
linux-workflow.yml
├── build-linux          (existing — Release + Debug + smoke test)
├── package-deb          (new — reuses build artifacts via download-artifact)
├── package-appimage     (new — reuses build artifacts, downloads appimagetool)
├── package-rpm          (new — container: rockylinux:9, full build from source)
├── package-arch         (new — container: archlinux:latest, full build from source)
└── package-flatpak      (new — ubuntu-latest, flatpak-builder, full build from source)

windows-workflow.yml
├── build-windows        (existing — Release + Debug + smoke test + ZIP)
└── package-msi          (new — depends on build-windows, WiX v4)

macos-workflow.yml
└── build-macos          (existing — Release + Debug + smoke test + DMG)  ← no changes
```

**Key design decisions:**
- DEB and AppImage **reuse** the Linux build artifacts (fast, ~2 min each).
- RPM, Arch, and Flatpak do **full builds from source** inside containers
  (their build systems expect to compile everything themselves).
- MSI **reuses** the Windows build artifacts (build-zip.ps1 already stages everything).
- All jobs upload artifacts via `actions/upload-artifact@v4`.

---

## Task 1 — DEB Package (ubuntu-latest)

**Job:** `package-deb` in `linux-workflow.yml`
**Depends on:** `build-linux` (downloads build artifacts)
**Script:** `packaging/Linux/Ubuntu/24_04/build-deb.sh --dry-run`

### Subtasks

- [ ] **1.1** Add `upload-artifact` step to `build-linux` job to export:
  - `bin/Release/jarvisAgent`
  - `dashboard/ui/dist/`
  - `workflow-editor/ui/dist/`
- [ ] **1.2** Add `package-deb` job:
  - `needs: build-linux`
  - `actions/download-artifact` to restore build outputs
  - Install `dpkg-deb` (already on ubuntu-latest)
  - Run `build-deb.sh --dry-run`
  - `actions/upload-artifact@v4` → `JarvisAgent-deb`
- [ ] **1.3** Verify: `.deb` appears in workflow artifacts, `dpkg-deb --info` passes

---

## Task 2 — AppImage (ubuntu-latest)

**Job:** `package-appimage` in `linux-workflow.yml`
**Depends on:** `build-linux` (downloads build artifacts)
**Script:** `packaging/Linux/AppImage/build-appimage.sh --dry-run`

### Subtasks

- [ ] **2.1** Add `package-appimage` job:
  - `needs: build-linux`
  - `actions/download-artifact` to restore build outputs
  - Download `appimagetool` from GitHub releases, `chmod +x`, place on PATH
  - Run `build-appimage.sh --dry-run`
  - `actions/upload-artifact@v4` → `JarvisAgent-AppImage`
- [ ] **2.2** Verify: `.AppImage` appears in artifacts, file is executable

---

## Task 3 — RPM (rockylinux:9 container)

**Job:** `package-rpm` in `linux-workflow.yml`
**Runs in:** `container: rockylinux:9` on `ubuntu-latest`
**Script:** `packaging/Linux/RPM/build-rpm.sh` (full build)

### Subtasks

- [ ] **3.1** Add `package-rpm` job:
  - `runs-on: ubuntu-latest`, `container: rockylinux:9`
  - Install deps: `dnf install -y gcc gcc-c++ make python3 python3-devel python3-pip zlib-devel nodejs npm rpm-build git`
  - Install premake5 (download binary tarball from GitHub releases)
  - Checkout with `submodules: recursive`
  - Run `build-rpm.sh` (full build)
  - `actions/upload-artifact@v4` → `JarvisAgent-rpm`
- [ ] **3.2** Verify: `.rpm` appears in artifacts, `rpm -qip` shows correct metadata

---

## Task 4 — Arch (archlinux:latest container)

**Job:** `package-arch` in `linux-workflow.yml`
**Runs in:** `container: archlinux:latest` on `ubuntu-latest`
**Script:** Uses `makepkg` with the PKGBUILD

### Subtasks

- [ ] **4.1** Add `package-arch` job:
  - `runs-on: ubuntu-latest`, `container: archlinux:latest`
  - `pacman -Syu --noconfirm base-devel python python-pip zlib nodejs npm git`
  - Install premake5 (download binary, or build from AUR)
  - Checkout with `submodules: recursive`
  - Create non-root build user (`makepkg` refuses to run as root)
  - Copy PKGBUILD + jarvisagent.install to a build dir
  - Patch PKGBUILD source to use local checkout instead of git clone
  - `su builduser -c "makepkg -s --noconfirm"`
  - `actions/upload-artifact@v4` → `JarvisAgent-arch`
- [ ] **4.2** Verify: `.pkg.tar.zst` appears in artifacts

**Note:** makepkg requires a non-root user. The job must create one and `chown`
the workspace before running makepkg.

---

## Task 5 — Flatpak (ubuntu-latest)

**Job:** `package-flatpak` in `linux-workflow.yml`
**Script:** `packaging/Linux/Flatpak/build-flatpak.sh`

### Subtasks

- [ ] **5.1** Add `package-flatpak` job:
  - Install `flatpak` and `flatpak-builder`
  - Add Flathub remote, install `org.freedesktop.Sdk//24.08` and
    `org.freedesktop.Sdk.Extension.node20//24.08`
  - Checkout with `submodules: recursive`
  - Run `build-flatpak.sh`
  - `actions/upload-artifact@v4` → `JarvisAgent-flatpak`
- [ ] **5.2** Verify: `.flatpak` bundle appears in artifacts

**Note:** This is the heaviest job (~20-30 min) because flatpak-builder downloads
the SDK runtime and builds everything from source inside its sandbox. Consider
making this job `if: github.ref == 'refs/heads/main'` to skip it on feature branches.

---

## Task 6 — MSI Installer (windows-latest)

**Job:** `package-msi` in `windows-workflow.yml`
**Depends on:** existing `build-windows` job (which already produces the ZIP staging dir)
**Script:** `packaging/Windows/11/build-msi.ps1`

### Subtasks

- [ ] **6.1** Modify `build-windows` to also upload the staging directory as an artifact
  (or combine MSI into the same job after ZIP)
- [ ] **6.2** Add MSI build step (or separate job):
  - Install WiX v4: `dotnet tool install --global wix`
  - Run `build-msi.ps1`
  - `actions/upload-artifact@v4` → `JarvisAgent-Windows-msi`
- [ ] **6.3** Verify: `.msi` appears in artifacts

**Simplest approach:** Add the WiX install + `build-msi.ps1` + artifact upload
as additional steps in the existing `build-windows` job (since `build-zip.ps1`
already ran and staged everything).

---

## Task 7 — Debian Source Package for Launchpad (PPA)

**Goal:** Produce a `.dsc` + `.orig.tar.gz` + `.debian.tar.xz` source package
that can be uploaded to a Launchpad PPA with `dput`. Launchpad builds the
`.deb` on its own build farm — we only upload source.

**PPA:** `ppa:beauman/marley` — `sudo add-apt-repository ppa:beauman/marley`
**premake5:** Already published in the same PPA (v5.0.16.2)
**Directory:** `packaging/Linux/Ubuntu/24_04/debian/`

### Background

The current `DEBIAN/` directory is for **binary** `.deb` packages built locally
with `dpkg-deb`. Launchpad requires a **source** package with a proper `debian/`
directory (lowercase) that uses `debhelper` build rules.

**Key challenge:** `premake5` is not in Ubuntu repos. The `debian/rules` must
either download the premake5 binary during build or build it from the bundled
source. Since Launchpad builds run in a clean chroot with no network access
(after source download), premake5 must be included in the source tarball.

### Subtasks

- [ ] **7.1** Create `debian/` directory structure:
  ```
  packaging/Linux/Ubuntu/24_04/debian/
  ├── changelog          # required — dpkg-parsechangelog reads this
  ├── compat             # debhelper compat level (or use debhelper-compat in control)
  ├── control            # source + binary package metadata
  ├── copyright          # DEP-5 machine-readable copyright
  ├── rules              # makefile — the actual build recipe
  ├── install            # list of files to install
  ├── postinst           # post-install hook (venv setup)
  ├── postrm             # post-remove hook (cleanup)
  └── source/
      └── format         # "3.0 (quilt)" or "3.0 (native)"
  ```

- [ ] **7.2** Write `debian/rules`:
  - `override_dh_auto_configure`: download or extract premake5, run `premake5 gmake`
  - `override_dh_auto_build`: `make -j$(nproc) config=release`, `npm install && npm run build` for both UIs
  - `override_dh_auto_install`: copy binary, UIs, scripts, docs, launcher to `debian/tmp/`
  - `override_dh_strip`: skip (or handle vendored static libs)
  - Handle premake5: include premake5 Linux binary in `debian/` or build from vendored source

- [ ] **7.3** Write `debian/control`:
  ```
  Source: jarvisagent
  Section: utils
  Priority: optional
  Maintainer: JC Technolabs <beaumanvienna@gmail.com>
  Build-Depends: debhelper-compat (= 13), gcc, g++, make,
   python3, python3-dev, zlib1g-dev, nodejs, npm, premake5
  Standards-Version: 4.6.2
  Homepage: https://github.com/beaumanvienna/JarvisAgent

  Package: jarvisagent
  Architecture: amd64
  Depends: python3 (>= 3.10), python3-pip, python3-venv, zlib1g, bash, ${shlibs:Depends}, ${misc:Depends}
  Description: Parallel AI-driven automation with C++ backend and React frontend
   JarvisAgent dispatches concurrent AI requests for bulk processing,
   supports visual DAG workflow editing, and provides a terminal UI
   and browser-based React dashboard.
  ```

- [ ] **7.4** Write `debian/changelog`:
  - Use `dch --create` or hand-write initial entry
  - Format: `jarvisagent (0.75-1) noble; urgency=medium`
  - Must match the target Ubuntu series (noble = 24.04)

- [ ] **7.5** Write `debian/copyright` (DEP-5 format):
  - Main license: GPL-3.0-only
  - Vendored libraries: list each with its own license

- [ ] **7.6** Write `debian/source/format`:
  - `3.0 (quilt)` for upstream tarball + debian patches
  - Or `3.0 (native)` if we treat this as a native Debian package

- [ ] **7.7** Create `build-launchpad.sh` helper script:
  - Extracts version from `premake5.lua`
  - Creates `.orig.tar.gz` from repo (excluding `.git/`, `node_modules/`, build artifacts)
  - Copies `debian/` into the source tree
  - Runs `debuild -S -us -uc` (unsigned) or `debuild -S` (signed with GPG)
  - Outputs `.dsc`, `.orig.tar.gz`, `.debian.tar.xz` ready for `dput`

- [x] **7.8** Handle premake5 in Launchpad builds: **RESOLVED**
  - premake5 (v5.0.16.2) is published in `ppa:beauman/marley`
  - `debian/control` lists `premake5` in `Build-Depends`
  - Launchpad PPA build recipe references `ppa:beauman/marley` as a dependency PPA
  - Users install with: `sudo add-apt-repository ppa:beauman/marley`
  - Source package files available at: `workflows/premake5/`

- [ ] **7.9** Test locally:
  ```bash
  # Build source package
  ./build-launchpad.sh

  # Test-build in a clean chroot (simulates Launchpad)
  sudo pbuilder build *.dsc
  ```

- [ ] **7.10** Add `package-launchpad` job to CI (optional):
  - Builds the source package (but does NOT upload — that requires GPG signing)
  - Uploads `.dsc` + tarballs as artifacts for manual `dput`

---

## Task 8 — CI Workflow Cleanup

### Subtasks

- [ ] **8.1** Add build artifact sharing: the `build-linux` job uploads
  `bin/Release/jarvisAgent` + both `ui/dist/` directories so downstream
  DEB and AppImage jobs can use `--dry-run` mode
- [ ] **8.2** Consider `concurrency` groups to cancel superseded runs
- [ ] **8.3** Consider running heavy jobs (Flatpak, RPM, Arch) only on `main`
  branch pushes to save CI minutes
- [ ] **8.4** Add version tag to artifact names (e.g. `JarvisAgent-0.75-deb`)

---

## Execution Order

| Phase | Tasks | Estimated effort |
|-------|-------|-----------------|
| 1 | Task 1 (DEB) + Task 2 (AppImage) | Small — reuse existing build, ~30 min |
| 2 | Task 6 (MSI) | Small — add steps to existing Windows job, ~20 min |
| 3 | Task 3 (RPM) + Task 4 (Arch) | Medium — container jobs with full builds, ~1 hr |
| 4 | Task 5 (Flatpak) | Medium — heavy build, SDK download, ~30 min |
| 5 | Task 7 (Launchpad) | Large — new debian/ dir, rules, testing, ~2-3 hrs |
| 6 | Task 8 (Cleanup) | Small — polish, ~20 min |

**Total estimated effort:** ~5-6 hours across multiple sessions.

---

## Artifact Summary (end state)

| Artifact name | Format | Source |
|---------------|--------|--------|
| `JarvisAgent-deb` | `.deb` | linux-workflow / package-deb |
| `JarvisAgent-AppImage` | `.AppImage` | linux-workflow / package-appimage |
| `JarvisAgent-rpm` | `.rpm` | linux-workflow / package-rpm |
| `JarvisAgent-arch` | `.pkg.tar.zst` | linux-workflow / package-arch |
| `JarvisAgent-flatpak` | `.flatpak` | linux-workflow / package-flatpak |
| `JarvisAgent-macOS-dmg` | `.dmg` | macos-workflow (existing) |
| `JarvisAgent-Windows-zip` | `.zip` | windows-workflow (existing) |
| `JarvisAgent-Windows-msi` | `.msi` | windows-workflow / package-msi |
| `JarvisAgent-launchpad-src` | `.dsc` + tarballs | linux-workflow (optional) |
