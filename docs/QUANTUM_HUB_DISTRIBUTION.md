<!-- (c) SR VINIX and contributors -->
<!-- SPDX-License-Identifier: MIT -->

# Quantum Hub distribution

`quantum-hub.exe` is the launcher distributed from the project website. It can
read a static HTTPS manifest, download a release ZIP, verify its SHA-256, reject
unsafe ZIP paths, extract it into the user's Quantum Hub directory, and launch
projects with that installed editor.

## Manifest

Host a JSON file such as `releases.json`:

```json
{
  "releases": [
    {
      "version": "0.8.1",
      "label": "Quantum 0.8.1",
      "platform": "windows-x64",
      "url": "https://downloads.example.com/quantum/0.8.1/windows-x64.zip",
      "sha256": "lowercase-sha256-of-the-zip",
      "size": 123456789
    }
  ]
}
```

Supported platform identifiers are `windows-x64`, `windows-arm64`, `linux-x64`,
`linux-arm64`, `macos-x64` and `macos-arm64`. A release without `platform` is
accepted on every platform.

The Hub requires HTTPS for the manifest and release archive. HTTP is accepted
only for `localhost` and `127.0.0.1` development servers. The checksum is
required and is checked before the archive is extracted.

## Release ZIP

The archive must contain one root folder. That folder must contain the runtime
editor distribution, including `quantum-editor.exe` on Windows and
`quantum-resource-server.exe` for project resource serving. The usual release
folder also contains `quantum.dll` and `engine/` beside the editor executable.

Example layout:

```text
Quantum-0.8.1/
  quantum-editor.exe
  quantum-resource-server.exe
  quantum.dll
  engine/
  templates/
```

The Hub installs each release under the platform's user data directory, for
example `%LOCALAPPDATA%\\Quantum\\Hub\\installs\\Quantum_0.8.1` on Windows.
Downloads are staged and promoted only after verification, so a cancelled or
damaged download does not become a selectable editor install.

## Building a branded Hub

Embed the website's manifest URL while building the standalone Hub:

```powershell
cmake -S . -B build -DQUANTUM_HUB_RELEASE_MANIFEST_URL=https://downloads.example.com/quantum/releases.json
cmake --build build --target quantum-hub --config Release
```

If no URL is embedded, the user can enter one in the Hub's **Installs** tab;
the value is saved in the Hub configuration for future launches.
