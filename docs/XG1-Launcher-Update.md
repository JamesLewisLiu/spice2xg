# Updating the XG1 launcher

Use the **32-bit Large Address Aware (LAA)** launcher on 64-bit Windows.
The standard 32-bit launcher has a 2 GiB address space. XG1 can exhaust its
contiguous free space when allocating video caches, even with free system RAM.
The 64-bit launcher cannot load the original 32-bit game modules.

New packages make both root `spice.exe` and `spice_laa.exe` the same LAA
executable. Copy the file your start script uses beside `game.dll`. Update
`spicecfg.exe` from the same package. Keep your existing configuration, saves
and compatible `clvsd.ax`. The non-LAA build remains in `extras/standard/spice.exe`.

```bat
spice_laa.exe -gdxg DM -w
```

Use `-gdxg GF` for GuitarFreaks. Add your existing service and PCBID options
as needed.

Older packages, including commit `95b08e4`, provide the LAA executable as
`extras/largeaddressaware/spice.exe`. That file must replace the executable
your start script actually runs. Copying the root `spice.exe` installs the
non-LAA build; extracting the package does not update an old `spice_laa.exe`
under a different filename.

Check these details before reporting another crash:

- The start script points to the file you updated.
- The first bootstrap line in `log.txt` says **Large Address Aware**.
- The executable matches the SHA256 checksum from the package. New packages
  provide `SHA256SUMS-launchers.txt` and the source commit in `BUILD_INFO.txt`.

For the `95b08e4` CI package, the LAA executable's SHA256 is:

```text
50e58ed52d26c7f8179559a03514de087bfc03199b7cc5df9c68159f03cc16a0
```

The date in the version string alone does not identify an exact executable.
When comparing crashes, include the executable checksum, full log and new dump.
