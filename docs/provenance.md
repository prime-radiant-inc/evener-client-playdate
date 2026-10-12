# Source provenance

This repository starts from the native client developed in
[Evener](https://github.com/prime-radiant-inc/evener).

- Source branch: `codex/playdate-client`.
- Source revision: `f25067ae4052c7cea48b0385d60aea923b28633f`.
- `tools/playdate-client/` became this repository's root.
- `tools/playdate-derp/enroll/` became `enroll/`.
- The client guide and physical qualification report moved under `docs/`.

The initial import is a source snapshot. Earlier development history remains in
Evener; unrelated Hub code, credentials, private bundles and diagnostic captures
are not imported into this repository's history. Vendor sources retain their
license notices and upstream provenance. No new license grant is introduced for
the first-party client code by this extraction.

## Extraction verification

A fresh GitHub clone builds the ARM and Simulator targets with Playdate SDK
3.1.2 and the ARM GNU 15.3.rel2 toolchain. Its ARM binary matches the physically
qualified build byte for byte: SHA-256
`7c19a3ecf994bddc7499748cf4e8d3b8243de7f7827f8bfe9f21f2138d1ba18a`.
The handheld runtime, build inputs, vendor files and existing check sources were
imported without changes. The extraction does not constitute a new hardware run.

Storage and socket checks pass, including an ASan/UBSan run. The enrollment
helper builds independently, and synthetic setup configuration passes without
contacting a provider. Evener's 24 acoustic interoperability cases and the
full browser/isolated-Hub pairing check pass using this standalone checkout.
Git history was scanned for credentials before publication; generated private
configurations and diagnostic artifacts are not part of the repository.
