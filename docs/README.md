# Documentation map

The [README](../README.md#documentation) table is the full index. Pages are grouped here by audience so you do not have to scan nineteen links for one task.

## Users

| Goal | Doc |
|------|-----|
| Install and first run | [README § Install](../README.md#install) |
| Flash, Download Mode, recovery | [using.md](using.md) (bundled as User Guide) |
| Supported models / caveats | [README § Supported devices](../README.md#supported-devices), [hardware.md](hardware.md) |

## App and CLI developers

| Goal | Doc |
|------|-----|
| End-to-end architecture | [architecture.md](architecture.md) |
| Odin session + planner rules | [protocol.md](protocol.md) |
| SwiftUI app behaviour | [app.md](app.md) (includes macOS HIG notes) |
| Agent, USB helper, security | [services.md](services.md) |
| XPC wire format | [service-wire.md](service-wire.md) |
| C API | [c-api.md](c-api.md) |
| CLI | [cli.md](cli.md) |

## Maintainers

| Goal | Doc |
|------|-----|
| Build, install, CMake options | [build-and-install.md](build-and-install.md) |
| Tests, fuzz, journal, CI | [testing.md](testing.md) |
| Sign, notarize, trademark (D-6) | [release.md](release.md) |
| GitHub About, topics, release SEO | [packaging/github-discovery.md](../packaging/github-discovery.md) |
| Roadmap (phases) | [roadmap.md](roadmap.md) |
| Status, coverage gaps | [status.md](status.md) |
| Requirement IDs (F-, S-, R-) | [requirements.md](requirements.md) |
| Contributing rules | [CONTRIBUTING.md](../CONTRIBUTING.md) |
| Changelog | [CHANGELOG.md](../CHANGELOG.md) |

## Repository layout

| Path | Contents |
|------|----------|
| `src/core/` | `libbifrost`: protocol, flash, C API (`include/bifrost/`, `src/`) |
| `src/service/` | Agent, USB helper, XPC wire, job journal |
| `src/cli/` | `bifrost-cli` |
| `src/app/` | SwiftUI app (`Sources/{App,Model,Bridge,Workspace}/`, `Resources/`) |
| `tests/` | Unit tests, `support/` simulator, `fixtures/`, `fuzz/` |
| `docs/` | All project documentation (this map) |
| `packaging/` | DMG, codesign, notarize, local install scripts |
| `scripts/` | `ci-local.sh`, `check-l10n.sh` |

Build trees stay **outside** the repo (see [build-and-install.md](build-and-install.md)).

## Conventions

- **One owner per topic** — merged pages: coverage and reliability used to be separate; they now live under [status.md](status.md) and [testing.md](testing.md).
- **UNVERIFIED** — anything not observed on hardware; canonical hardware log is [hardware.md](hardware.md).
- **Mermaid** — diagrams render on GitHub and in many Markdown viewers.