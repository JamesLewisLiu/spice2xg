# XG1 / XG1CN Quick Start

spice2xg runs GuitarFreaks & DrumMania XG (Japan) and XG (China) using the original game DLLs and data.

## Install

Back up your existing launcher, settings, and saves. Place the following files beside `game.dll` and `boot.dll`:

- The **32-bit** spice2xg launcher, named `spice.exe`.
- `spicecfg.exe` from the same build.
- An XG1-compatible **32-bit** `clvsd.ax` for video playback.

On 64-bit Windows, use the **Large Address Aware (LAA)** launcher. In the full distribution archive, copy `extras/largeaddressaware/spice.exe` into the game folder. If your build provides `spice_laa.exe` directly, rename that file to `spice.exe`.

XG1 requires a 32-bit launcher. `spice64.exe` cannot run these game modules. The original `gdxg.exe` is not required.

The decoder is loaded from beside `spice.exe` without `regsvr32`. Use an AX compatible with XG1; decoders for other GFDM versions may expose different classes.

Typical layout:

```text
Game/
  data/
  binspice/
    spice.exe
    spicecfg.exe
    clvsd.ax
    game.dll
    boot.dll
    ...other original game DLLs...
```

## Configure controls

Open a command prompt in the binary folder and run:

```bat
spicecfg.exe -gdxg GF -runas user
```

Use `-gdxg DM` for DrumMania. Bind your instrument controls, Coin, Start, Test, and Service. The configurator selects GitaDora and detects the region and date from the original modules. Opening `spicecfg.exe` without arguments in an XG1 installation defaults to GF.

## Launch

```bat
spice.exe -gdxg GF -url "http://your-server:8080/" -p YOUR_20_CHARACTER_PCBID
```

Replace the server URL and PCBID with your own settings. Use `-gdxg DM` for DrumMania. Add `-w` for windowed mode, or `-runas user` to run without requesting elevation.

| Instrument | Model |
| --- | --- |
| GF | J33 |
| DM | J32 |

Japan/China selection and the datecode are detected automatically. No identity XML is required. Use game files from the intended region; changing the instrument does not convert game data between regions.

For a basic local startup of the Japanese version, use `-ea` instead of `-url`. The internal server does not provide full online services. The Chinese version keeps its native local Session system and does not use EA3; online card support is not added.

Wait for initialization, then check the inputs in TEST mode before playing.

## Settings and saves

Each instrument uses its own directory beside the launcher:

```text
DEV/
  GF/
    spicetools.xml
    CONF/NVRAM/
    CONF/RAW/
    TMP/
    CACHE/PROXY/
  DM/
    ...same layout...
```

`spicetools.xml` contains Spice's controls and options, including Service URL and PCBID. Once saved there, these settings can be omitted from the command line.

An AVS XML is optional. If you need one, place it at `DEV/GF/avs-config.xml` or `DEV/DM/avs-config.xml`, or select a file with `-v path/to/avs-config.xml`. Explicit `fs/root/device`, `fs/nvram/device`, and `fs/raw/device` paths are honored. Optional `fs/tmp/device`, `fs/cache/device`, and `fs/proxy/device` paths control temporary storage. Use scalar devices; this legacy backend does not support `fs/mounttable`.

Without an AVS override, assets are read from the parent of the binary folder and storage uses the layout above. `-cfgpath` remains available for a custom Spice settings file. Existing identity XML arguments are still accepted for compatibility.

## Troubleshooting

- **No video:** check the local 32-bit `clvsd.ax` and the `gdxg-codec` messages in `log.txt`.
- **Missing data:** check the parent directory layout or your AVS root device.
- **Japanese texture corruption:** use a current build containing the XG1 texture compatibility fix.
- **Memory errors:** confirm that you installed the LAA launcher. More address space helps, but does not guarantee that every memory-related crash is resolved.

When reporting an issue, include `log.txt`, the build version, the instrument/region, and a crash dump if available.
