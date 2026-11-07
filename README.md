# PFAviz++

A performance rewrite of [Piano From Above](https://github.com/brian-pantano/PianoFromAbove) (Brian Pantano, 2010) for black MIDI. Notes and the keyboard are drawn exactly as Piano From Above draws them, pixel for pixel; loading, storage, playback and rendering are rebuilt for files with billions of notes.

## Summary

- Has massively improved MIDI loading: files with billions of notes load in seconds to minutes, using far less memory.
- Opens compressed and archived MIDIs (7z, zip, xz and more), including layered files of trillions of notes.
- Draws as fast whatever the note count, with no note limit, and the same pixels as the original.
- Plays smoothly on its own threads, with KDMAPI support and optional limits for synths that fall behind.
- Can simulate how the original Piano From Above would lag on your computer.
- Adds a configurable stats box and graphs of notes per second, polyphony and tempo.
- Adds optional effects: lighting, glow, flat and semi-flat looks, a hit line, and saved effect profiles.
- Renders songs to MP4 video.
- Adds 128- and 256-key views, a 31-EDO keyboard and tuning, dark mode, track palettes, and drag and drop.

For how each of these works, and the settings behind them, see [FEATURES.md](FEATURES.md).

## Requirements

- Windows 10 or later, 64-bit, with a Direct3D 9Ex graphics driver.
- Optional: a KDMAPI synth, such as [OmniMIDI](https://github.com/KeppySoftware/OmniMIDI), for fast synth output; `d3dx9_43.dll` (DirectX End-User Runtime) for the original's exact text rendering, otherwise a GDI fallback is used; [ffmpeg](https://ffmpeg.org/download.html) for video rendering.

## Usage

Open a song with File > Play Song, by dropping it on the window, or by passing it on the command line:

```
PFAviz++.exe "song.mid"
```

Settings are kept in `%APPDATA%\PFAviz++\config.ini`. More on the settings, the window size and test switches is in [FEATURES.md](FEATURES.md).

## Building

You need [w64devkit](https://github.com/skeeto/w64devkit) (GCC 16 was used). Visual Studio is not required.

Compressed and archived songs are read with [libarchive](https://www.libarchive.org) and its compression libraries (zlib, bzip2, xz, zstd and lz4), built static into `vendor/archive` once, from the w64devkit shell, before the first `make`. The script downloads pinned releases (it needs `curl`, which Windows 10 and later include), checks their SHA-256 and builds them with w64devkit's CMake:

```
sh build-deps.sh
```

Then:

```
make
```

The output is `build/PFAviz++.exe`, a static executable with no extra DLLs. `make debug=1` makes a debug build in `build/debug/`, and `make native=1` builds for the current CPU.

## Credits

- Based on [Piano From Above](https://github.com/brian-pantano/PianoFromAbove) by Brian Pantano. Its drawing, key colouring and pairing rules are reproduced exactly; its icons and splash image are used as the program's own. Piano From Above's own source is not included here.
- The splash song, *Godowsky – Study of Chopin Etude Op. 10, No. 1*, is by Bernd Krueger ([piano-midi.de](http://www.piano-midi.de)), licensed under [CC BY-SA 3.0 DE](https://creativecommons.org/licenses/by-sa/3.0/de/deed.en).

## License

PFAviz++ is released under the [GNU General Public License v3.0](LICENSE). Piano From Above itself is © 2010 Brian Pantano and is not covered by this license; its source is not included here.
