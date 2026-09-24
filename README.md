# alien-externals

A collection of **132 audio externals and effects for [Pure Data](https://puredata.info)** —
synths, physical models, chaotic/generative voices, and effects. Every object is a
signal (`~`) external written in portable C.

This repository ships **only C sources** plus everything needed to build them.
Compiled binaries are not checked in — you build them for your own platform (see below).

## What's here

| File | Purpose |
| --- | --- |
| `alien_*~.c` | 132 external sources, one Pd class each (class name == file basename) |
| `alien.h` | Header-only DSP toolkit shared by many of the externals (oscillators, filters, envelopes, chaos maps, reverb, FFT, …) |
| `Makefile` | Build config listing every source ([pd-lib-builder](https://github.com/pure-data/pd-lib-builder)) |
| `Makefile.pdlibbuilder` | Vendored pd-lib-builder (the cross-platform build engine) |

## Requirements

- A C compiler (`gcc`/`clang` on macOS & Linux, MinGW on Windows).
- GNU `make` (>= 3.81).
- The Pure Data headers — chiefly `m_pd.h`. These ship inside every Pd install; the
  build finds them automatically when you point it at your Pd (`PDDIR=...`), or you
  can pass their location with `PD_INCLUDES=-I/path/to/pd/src`.

No other dependencies — `alien.h` is self-contained (only `<math.h>`, `<stdint.h>`,
`<string.h>`), so each external compiles standalone with no linking against shared
objects.

## Building

Build every external for the current platform:

```bash
make                        # auto-detects Pd if installed in a standard location
```

If `m_pd.h` is not found, point the build at your Pd. Examples per platform:

### macOS

```bash
# Point at your Pd.app and build a universal (Apple Silicon + Intel) binary:
make PDDIR="/Applications/Pd-0.56-1.app/Contents/Resources" arch="arm64 x86_64"
```

Produces `*.pd_darwin` externals.

### Linux

```bash
sudo apt install puredata-dev      # provides m_pd.h  (Debian/Ubuntu)
make
# or, against a custom Pd source tree:
make PD_INCLUDES=-I/path/to/pd/src
```

Produces `*.pd_linux` externals.

### Windows (MinGW / MSYS2)

```bash
make PDDIR="/c/Program Files/Pd" PDBINDIR="/c/Program Files/Pd/bin"
```

Produces `*.dll` externals. (On Windows the build also needs `pd.dll`/`libpd`
import library from your Pd install, which `PDBINDIR` supplies.)

### Other targets

```bash
make CFLAGS="-O2"           # override optimisation flags
make clean                  # remove all build products (.o and binaries)
make help                   # pd-lib-builder's own help
```

Full option reference lives at the top of `Makefile.pdlibbuilder`.

## Installing

```bash
make install                                  # default: ~/Documents/Pd/externals
make install objectsdir="$HOME/Library/Pd"    # custom location
```

Do **not** use `sudo` for a home install — it sets `HOME=/var/root` and the files
land in root's home. After installing, add the parent of the installed folder to
Pd's search path (Pd → Preferences → Path), then the objects are available as
e.g. `[alien_bass~]`, `[alien_gendy~]`, `[alien_xfx01~]`.

## Using an external

Each source builds to one Pd class named after the file (minus the `.c`). For
example `alien_bass~.c` → the `[alien_bass~]` object. Create it in a patch and
connect its inlets/outlets; most take a base pitch/parameter on the left inlet and
one or more control values on the right inlet(s). See the top-of-file comment in
each `.c` for its specific inlets and arguments.

## License

See individual source files for authorship. The vendored `Makefile.pdlibbuilder`
is public domain (Katja Vetter).
