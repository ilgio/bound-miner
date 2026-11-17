# Making a release

Releases are built by GitHub Actions ([release.yml](../.github/workflows/release.yml)), never on
someone's own PC, so anyone can check that a download came from this repository's source.

## Steps

1. **Pick the version** and set it in one place, `project(bound VERSION ...)` in
   [CMakeLists.txt](../CMakeLists.txt). Everything else takes it from there: `--version`, the user
   agent peers see, the Windows file properties and the package names. Before mainnet launched the
   versions were 0.x, marked as pre-releases; from 1.0.0 they're full releases.
2. **Write the release notes** in `docs/release-notes/v<version>.md`: what's new, what to know
   before upgrading. They become the release's description.
3. **Try it** (optional, recommended): *Actions > Release > Run workflow*. That's a dry run: it
   builds and checks both packages and keeps them as the run's artifacts, and publishes nothing.
4. **Commit, tag and push:**

       git commit -am "BOUND 0.10.0"
       git tag v0.10.0
       git push origin main v0.10.0

5. **Wait for the workflow** (*Actions > Release*), 15 to 30 minutes. It:
   - checks that the tag, `CMakeLists.txt` and the release notes agree;
   - builds the Windows zip (with the wallet app) and the Linux tarball, runs every test on those
     exact programs, and checks that they run on their own: on Windows with nothing else on PATH,
     on Linux needing nothing but glibc 2.35;
   - writes `SHA256SUMS`, attests every file (below), and drafts a release with all of it.
6. **Publish:** open the draft under *Releases*, read it, download a package and try it, then press
   *Publish release*.

If anything fails, nothing is published. Fix it, then move the tag to the fixed commit
(`git tag -f v0.10.0` and `git push -f origin v0.10.0`) and delete the old draft, if there is one.
Once a release is published, never move its tag: make a new version instead.

## What a download proves

- **`SHA256SUMS`** lets anyone check that a file arrived intact.
- **The attestations** (GitHub's artifact attestations, signed through Sigstore) prove that each file was built by this repository's release workflow,
  from the tagged commit, and hasn't been changed since. Nobody holds a signing key that could be
  stolen. Anyone can check one with `gh attestation verify <file> --repo Sonic-Bound/Bound`.

What it doesn't prove yet:
- **Code signing on Windows.** That needs a certificate, which costs money each year. Until then,
  Windows SmartScreen warns about the programs.
- **Reproducible builds.** Bitcoin Core's are built in Guix by several people, who check that they
  get the same bytes, so nobody needs to trust GitHub. That's worth doing before mainnet holds real
  value.

## Where the pieces are

| File | What it does |
|---|---|
| [`.github/workflows/release.yml`](../.github/workflows/release.yml) | the workflow above |
| [`contrib/release/package-windows.ps1`](../contrib/release/package-windows.ps1) | builds, tests, installs, checks and zips on Windows; runs on a PC too, from Developer PowerShell for VS 2022 |
| [`contrib/release/package-linux.sh`](../contrib/release/package-linux.sh) | the same for Linux, with vcpkg's static libraries |
| [`contrib/release/README.txt`](../contrib/release/README.txt) | the README inside each package |
| `install()` rules in [CMakeLists.txt](../CMakeLists.txt) | what goes into a package: the programs, the DLLs and Qt plugin on Windows, the licenses |
