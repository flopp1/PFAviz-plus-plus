# PFAviz++

A performance rewrite of [Piano From Above](https://github.com/brian-pantano/PianoFromAbove) (Brian Pantano, 2010) for black MIDI. Notes and the keyboard are drawn exactly as Piano From Above draws them, pixel for pixel; loading, storage, playback and rendering are rebuilt for files with billions of notes.

## Features

**Loading**
- Files load on a background thread with a progress and statistics popup: per-stage times, throughput, memory in use and its peak.
- The loader is memory-mapped and multi-threaded. A 100 M-note file loads in about 2.6 s on a 6-core CPU; files that do not fit in memory are read once, at the disk's sequential speed.
- Events are stored LZ4-compressed (0.2–0.4 bytes per event on black MIDIs), and notes that can never show are dropped: a 1.1 billion-note, 6.3 GB file takes about 270 MB once loaded.
- **Compact visual store** (Performance tab) packs the notes about 25–35 % smaller, at about the same drawing speed.
- Notes transposed past key 127 (a common black MIDI artefact) are read as stock Piano From Above reads them and dropped, instead of throwing the rest of their track out of step; velocities and controller values past 127 are clamped.
- **Ignore weird notes** (Performance tab, off by default): a note that a later note-off would pair a second time keeps its first pairing. Songs with such notes then load with one pairing pass instead of two (*Night of Nights*, 2.3 B notes: 221 s → 139 s, peak memory 17.7 → 11.9 GB). Those notes end at their first note-off, unlike the original.

**Drawing**
- Each frame finds, per key, only the notes that own a pixel row, so drawing cost depends on the window size, not the note count. There is no note limit.
- An optional **LOD cache** (on by default) keeps, for the current window size and note speed, only the notes that can show, built in the background around the playhead. Dense songs draw several times faster with the same pixels.
- **Hidden channels** get their own drawing data, built in the background ("Applying hidden channels..." until ready), so hiding most of a black MIDI's tracks makes it faster.
- **128 keys** by default ("Show all 88 keys" is still there).

**Playback**
- Audio runs on its own decoder and sender threads, off a microsecond clock, independent of the frame rate. KDMAPI (OmniMIDI) is used when available, WinMM otherwise.
- Every event is sent by default. Optional limits (Performance tab), for synths that fall behind:
  - **Skip quiet notes while the synth lags**: above a lag you choose, quiet note-ons go first, rising to every note-on at 4 times that lag.
  - **Always skip notes quieter than velocity N** (default 20), with their note-offs.
  - **Collapse stacked notes**: one voice per channel, key and tick, lasting until the last of the stacked notes ends. RFTMIS rev10 sends 42 % fewer note-ons.
  - **Limit sending to N messages/s**: looks 50 ms ahead and skips the quietest note-ons first so the rest fit; within the last velocity it thins evenly, so songs with one velocity still play partly. Note-ons stay within N/2 per second; total messages average N but can briefly reach about 1.5 N as earlier notes end.
- Mute, volume, pause, stop, seeking (with controller and program chase) and the metronome behave as in the original.

**Statistics**
- Each line of the stats box is chosen in Visual > **Statistics...**; with every line off the box is not drawn. The lines:
  - Time, frame rate and score (on by default: time and score).
  - Notes passed, NPS (note-ons in the last second) and polyphony (note-ons minus the note-offs that release a held note of the same track, channel and key), from 1 ms tables built while the notes are paired at load time (on by default; not counted while all three are off).
  - Synth: messages sent, skipped and collapsed per second, the sender's lag and the skipping threshold.
  - Frame time, split into logic, notes, the rest of the scene and Present.

**Video rendering** (Performance tab, "Render video...")
- Renders the open song from start to end to an MP4, off-screen at any resolution up to 8192 × 8192 and 1–240 fps, with x264 (CPU) or a hardware encoder (NVIDIA NVENC, AMD AMF or Intel QSV; H.264 or HEVC), a quality setting (CRF, CQ or QP; default 20) and a speed preset.
- Frames are converted to YUV 4:2:0 (BT.709) on the GPU and read back a few frames behind the GPU, so ffmpeg only encodes. At 1080p60 on an RTX 3060: about 290 fps with x264 veryfast, 540 fps with NVENC.
- A frame is what the window shows (keyboard and notes with the current view, note speed, colours and hidden channels) plus, optionally, the stats box (its chosen lines, less the synth and frame-time ones). There is no sound.
- Needs `ffmpeg.exe` in the current folder, next to `PFAviz++.exe`, or on the PATH. The progress popup shows a preview; Cancel keeps a playable video of the part done so far.

## Requirements

- Windows 10 or later, 64-bit, with a Direct3D 9Ex graphics driver.
- Optional: [OmniMIDI](https://github.com/KeppySoftware/OmniMIDI) (KDMAPI) for fast synth output; `d3dx9_43.dll` (DirectX End-User Runtime) for the original's exact text rendering, otherwise a GDI fallback is used; [ffmpeg](https://ffmpeg.org/download.html) for video rendering.

## Usage

Open a song with File > Play Song, or pass it on the command line:

```
PFAviz++.exe "song.mid"
```

Settings are kept in `%APPDATA%\PFAviz++\config.ini`.

Environment variables for timed test runs: `PFAVIZ_MUTE=1`, `PFAVIZ_UNCAPPED=1`, `PFAVIZ_NOTESPEED=x`, `PFAVIZ_MAXIMIZE=1`, `PFAVIZ_FRAMELOG=file` (frame times every half second), `PFAVIZ_GDITEXT=1` (the GDI text fallback even with D3DX installed), `PFAVIZ_EXIT_AFTER=seconds` (closes the window without saving the configuration) and `PFAVIZ_RENDER=out.mp4` (renders the song with the saved video settings, then closes; with `PFAVIZ_ENCODER=n`, the n-th encoder of the list instead, `PFAVIZ_CAPTUREDEPTH=n` frames in flight, `PFAVIZ_BGRXPIPE=1` BGRX frames converted by ffmpeg).

## Building

You need [w64devkit](https://github.com/skeeto/w64devkit) (GCC 16 was used). Visual Studio is not required.

```
make
```

The output is `build/PFAviz++.exe`, a static executable with no extra DLLs. `make debug=1` makes a debug build in `build/debug/`, and `make native=1` builds for the current CPU.

## Credits

- Based on [Piano From Above](https://github.com/brian-pantano/PianoFromAbove) by Brian Pantano. Its drawing, key colouring and pairing rules are reproduced exactly; its icons and splash image are used as the program's own. Piano From Above's own source is not included here.
- The splash song, *Godowsky – Study of Chopin Etude Op. 10, No. 1*, is by Bernd Krueger ([piano-midi.de](http://www.piano-midi.de)), licensed under [CC BY-SA 3.0 DE](https://creativecommons.org/licenses/by-sa/3.0/de/deed.en).

## License

PFAviz++ is released under the [GNU General Public License v3.0](LICENSE). Piano From Above itself is © 2010 Brian Pantano and is not covered by this license; its source is not included here.
