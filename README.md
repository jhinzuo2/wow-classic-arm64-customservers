# World of Warcraft Classic ARM64 & Custom Server Toolkit

A centralized repository and tooling hub for running native ARM64 and Wine-compatible World of Warcraft Classic client builds, custom launchers, and proxy configurations.

---

## Overview

This repository provides custom launchers, build configurations, and proxy connection setups to run World of Warcraft Classic client builds natively on **ARM64** hardware (such as Windows on ARM or Linux ARM devices) as well as under **Wine** cross-platform environments, hooked up seamlessly to custom private servers via HermesProxy.

---

## Included Launchers & Modules

| Tool / Submodule | Target Version | Platform / Environment | Description |
| :--- | :--- | :--- | :--- |
| **ClassicForeverLauncher** | `1.60.1.70009` | ARM64 / Wine / x64 | Custom launcher for Classic builds with command-line argument overrides. |
| **ArctiumClassicEra** | `1.14.2.42597` | ARM64 / Wine / x64 | Arctium-style custom launcher framework adapted for Classic Era. |
| **HermesProxy** *(Submodule)* | N/A | Cross-Platform | Handles custom server proxy redirection and authkey connections. |

---

## ClassicForeverLauncher Startup Arguments

When running `ClassicForeverLauncher` via command line or shortcuts, you can utilize the following flags to manage execution speed, diagnostics, delays, and architecture settings:

```cmd
ClassicForeverLauncher.exe --arm64 --delay 30 --nodiag --fast 32
```

### Argument Breakdown
* `--arm64` — Forces ARM64 architecture execution paths and optimizations.
* `--delay 30` — Introduces a 30-second delay (useful for waiting on proxy or network daemon initialization).
* `--nodiag` — Disables diagnostic reporting and telemetry hooks.
* `--fast 32` — Adjusts execution timing and fast-path compatibility parameters.

---

## Getting Started & Installation

1. **Clone the repository recursively** (to automatically pull submodules like HermesProxy):
   ```bash
   git clone --recursive https://github.com/jhinzuo2/wow-classic-arm64-customservers.git
   ```
   *(If you already cloned normally, run `git submodule update --init --recursive`)*

2. **Directory Setup:**
   * Place your compiled launcher executables directly into your target World of Warcraft directory containing the corresponding client version build (`1.60.1.70009` or `1.14.2.42597`).
3. **Compatibility:**
   * Designed for native ARM64 host environments or Wine/compat layers on Linux-based setups.

---

## Compilation from Source

If you need to recompile the launchers using MinGW-w64 / MSYS2 toolchains, use the provided build batch scripts inside each launcher directory:
* Ensure your toolchain path is configured (`aarch64-w64-mingw32-gcc` or `x86_64-w64-mingw32-gcc`).
* Run the respective `build.bat` script to output binaries straight to the `build\` folder.

---

## License
*No license applied (All rights reserved / Default copyright applies).*