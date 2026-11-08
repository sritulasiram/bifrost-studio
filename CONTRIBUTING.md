# Contributing

Thanks for helping. This tool writes to people's phones, so the rules favour safety over convenience.

## The rules

1. **Fail safe.** Refuse on any ambiguity. If you can't prove a step is safe, make it an error.
2. **Never claim device behaviour works without a test or a source citation.** Mark anything else UNVERIFIED in the docs. Hardware results go only in [docs/project/hardware.md](docs/project/hardware.md). The simulator proves the host is consistent; it does not prove how a real phone behaves.
3. **No destructive features** (re-partitioning, NAND erase, user-data erase) unless the owner has approved a design first. Open an issue to discuss before writing code.
4. **Never send `0x64/1`.** Its meaning is disputed, and the simulator fails any test that sends it.
5. **Modem-type targets (PIT binary type 1) stay refused** until hardware settles their end-of-sequence layout.
6. **A flash is the first and only Odin session of a Download Mode entry.** It never resumes (F-40).
7. **Don't copy code** from odin4 (Apache-2.0), Thor (MPL-2.0) or Heimdall. Use them as behavioural references only, and write your own implementation. If you used one to learn something, cite it in a comment.
8. **No firmware downloaders.**

Requirement IDs such as F-40 and S-4 are stable and cited by code and tests; see [docs/project/requirements.md](docs/project/requirements.md). Don't renumber them.

## Setting up

Build and test steps are in [docs/guides/build-and-install.md](docs/guides/build-and-install.md) and [docs/guides/testing.md](docs/guides/testing.md). In short:

- Build into a directory **outside** the repository.
- The build must stay universal (or x86_64); never arm64-only.
- All tests must pass with no compiler warnings (`-Wall -Wextra -Wpedantic`), and the exported symbols must stay `bf_*` only.

## Making a change

- **Protocol or planner change:** add a simulator case first, then the code. Update [docs/design/protocol.md](docs/design/protocol.md), and keep the claim UNVERIFIED until it runs on a real phone.
- **C API change:** update `bifrost.h` and [docs/reference/c-api.md](docs/reference/c-api.md). Bump `BF_API_VERSION` on any incompatible change.
- **Wire (XPC) change:** bump `kWireVersion` and update [docs/reference/service-wire.md](docs/reference/service-wire.md).
- **Parser change** (PIT, package, LZ4, names): run the fuzzers, and add any crash input to the corpus as a regression seed.
- **Behaviour or status change:** update the doc page that owns it; see the conventions in [docs/README.md](docs/README.md).

## Hardware reports

The most useful contribution is a result from a phone we haven't tested. Please include the model and firmware, the PIT (`bifrost-cli read-pit`), the result, and the app log or `--machine` output. Don't flash a phone you can't afford to recover.

## Commits and pull requests

By contributing, you agree that your contribution is licensed under the project's [MIT License](LICENSE). Keep changes small and focused, and explain the reason for each. Describe how you tested it, and say plainly what is not verified on hardware.
