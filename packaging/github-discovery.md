# GitHub discovery (About, topics, releases)

Keep the public repo easy to find on GitHub and in web search. The README is the main SEO surface; these settings reinforce the same keywords.

## About (required)

| Field | Value |
|-------|--------|
| **Description** | `Samsung firmware flasher for Mac: native Odin alternative for macOS. Flash Galaxy .tar.md5 (BL, AP, CP, CSC) in Download Mode. Beta.` |
| **Website** | `https://github.com/sritulasiram/bifrost-studio/releases/latest` |
| **Topics** | `samsung`, `samsung-firmware`, `galaxy`, `odin`, `odin-alternative`, `heimdall`, `download-mode`, `firmware-flasher`, `flash-firmware`, `macos`, `mac`, `swift`, `swiftui`, `android`, `usb`, `libusb`, `pit`, `bifrost` |

Do **not** use the old `…/bifrost/releases/…` URL (redirects work but split SEO).

Optional: disable **Wiki** if unused (Settings → General → Features).

## Release notes template

When tagging `vX.Y.Z`, open the release body with one discovery sentence, then highlights:

```markdown
Native **Samsung Galaxy firmware flasher for macOS** — **Odin alternative** for Download Mode `.tar.md5` packages (BL, AP, CP, CSC). Universal build (macOS 13+, Apple Silicon + Intel).

### Highlights
…
```

For **v1.1.0**, sync from `packaging/release-notes/v1.1.0.md` if you edit notes in git.

## Apply with GitHub CLI

After `gh auth login`:

```sh
gh repo edit sritulasiram/bifrost-studio \
  --homepage 'https://github.com/sritulasiram/bifrost-studio/releases/latest' \
  --description 'Samsung firmware flasher for Mac: native Odin alternative for macOS. Flash Galaxy .tar.md5 (BL, AP, CP, CSC) in Download Mode. Beta.' \
  --enable-wiki=false

gh api -X PUT repos/sritulasiram/bifrost-studio/topics \
  -f names='["samsung","samsung-firmware","galaxy","odin","odin-alternative","heimdall","download-mode","firmware-flasher","flash-firmware","macos","mac","swift","swiftui","android","usb","libusb","pit","bifrost"]'

gh release edit v1.1.0 --notes-file packaging/release-notes/v1.1.0.md
```