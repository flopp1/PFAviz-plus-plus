# PFAviz++

A performance rewrite of [Piano From Above](https://github.com/brian-pantano/PianoFromAbove) (Brian Pantano, 2010) for black MIDI. Notes and the keyboard are drawn exactly as Piano From Above draws them, pixel for pixel; loading, storage, playback and rendering are rebuilt for files with billions of notes.

## Features

**Loading**
- Files load on a background thread with a progress and statistics popup: per-stage times, throughput, memory in use and its peak.
- The loader is memory-mapped and multi-threaded. A 100 M-note file loads in about 2.6 s on a 6-core CPU; files that do not fit in memory are read once, at the disk's sequential speed.
- Events are stored LZ4-compressed (0.2–0.4 bytes per event on black MIDIs), and notes that can never show are dropped: a 1.1 billion-note, 6.3 GB file takes about 270 MB once loaded.
- Peak memory while loading stays close to what the song takes once loaded. The event stream is built a window at a time, and the count pass already works out which notes no note-off will pair and when long notes end, so pairing finishes those at once instead of holding back every later note of their key. *Night of Nights* (17.7 GB, 2.3 billion notes): peak 10.4 → 4.8 GB, 3.9 GB once loaded; *Destruction 321 SE*: 2.7 → 1.7 GB.
- **Compact visual store** (Performance tab, on by default) keeps the notes LZ4-compressed in groups of 2048 per key, decoding only the groups a frame draws: 20–36 % less memory for the notes (*Night of Nights*: 2,260 → 1,635 MB), with the same pixels. Frames read from it while scrubbing (a jump every frame) cost 0–25 % more CPU, still under about 6 ms on *Night of Nights* and *Destruction 321 SE*.
- Notes transposed past key 127 (a common black MIDI artefact) are read as stock Piano From Above reads them and dropped, instead of throwing the rest of their track out of step; velocities and controller values past 127 are clamped.
- **Weird notes end at their own note-off.** When more than 10 notes stack up on one channel and key in a track, the original pairs the next note-off with the latest unpaired note of that key on *any* channel; a note taken from another channel's stack is then paired again by its own channel's note-off. The original draws it to that later note-off but stops drawing it (and unlights its key) at the first one, so it seems to end early, often the moment it reaches the keyboard. Here it lasts to its later note-off, which in *Night of Nights* is its own channel's for 1,146 of its 1,174 such notes (a note-off on another channel cannot end it). Songs with them also load with one pairing pass instead of two (*Night of Nights*, from an SSD: loading 123 s, pairing 95 s).
- **End weird notes at their first note-off** (Performance tab, off by default): those notes keep their first pairing instead, so they are drawn short.

**Drawing**
- Each frame finds, per key, only the notes that own a pixel row, so drawing cost depends on the window size, not the note count. There is no note limit.
- Consecutive one-colour rects of a key that meet edge to edge are drawn as one, with the same pixels. In dense passages, where every pixel row has its own note, this removes almost all of them (the end of *rftmis*: 69,400 rects per frame → 373, frame time 1.55 → 0.87 ms).
- An optional **LOD cache** (on by default) keeps, for the current window size and note speed, only the notes that can show, built in the background around the playhead. Dense songs draw several times faster with the same pixels.
- Optional **GPU instancing for notes** (Performance tab, off by default): each note goes to the GPU as one 32-byte instance instead of two quads (160 bytes), and a vertex shader builds its rects. It is not pixel-exact: over the test harness's 90,000 frames, 171 differ from the original, each by one colour level (out of 255) along one or two pixel columns of a note's gradient; note edges are never off. With a long note span it saves about a quarter of the frame time (rftmis at 3 s: 0.49 → 0.36 ms median); at short spans it is about even.
- **Hidden channels** get their own drawing data, built in the background ("Applying hidden channels..." until ready), so hiding most of a black MIDI's tracks makes it faster.
- **128 keys** by default ("Show all 88 keys" is still there).
- The **note speed slider** is logarithmic and covers the whole range: 10 at the left, 0.06 (the default) in the middle, 0.005 at the right.

**Playback**
- Audio runs on its own decoder and sender threads, off a microsecond clock, independent of the frame rate. KDMAPI (OmniMIDI) is used when available, WinMM otherwise.
- Every event is sent by default. Optional limits (Performance tab), for synths that fall behind:
  - **Skip quiet notes while the synth lags**: above a lag you choose, quiet note-ons go first, rising to every note-on at 4 times that lag.
  - **Always skip notes quieter than velocity N** (default 20), with their note-offs.
  - **Collapse stacked notes**: one voice per channel, key and tick, lasting until the last of the stacked notes ends. RFTMIS rev10 sends 42 % fewer note-ons.
  - **Limit sending to N messages/s**: looks 50 ms ahead and skips the quietest note-ons first so the rest fit; within the last velocity it thins evenly, so songs with one velocity still play partly. Note-ons stay within N/2 per second; total messages average N but can briefly reach about 1.5 N as earlier notes end.
- When one thread cannot decode a dense passage in time, decoding spreads over more threads, up to the Performance tab's thread setting (a change applies at once), and back to one when the passage ends. The visuals stay exact; audio may differ slightly from one thread's: with collapsing on, a tick split between threads can sound a key once per thread, and the send budget is shared between them. Night of Nights' busiest second (224 M events) decodes in 0.18 s on 16 threads with collapsing, the budget and the velocity floor on, against 0.96 s on one.
- Mute, volume, pause, stop, seeking (with controller and program chase) and the metronome behave as in the original (clicks inside a parallel stretch are left out).

**Statistics**
- Each line of the stats box is chosen in Visual > **Statistics...**; with every line off the box is not drawn. The lines:
  - Time, frame rate and score (on by default: time and score).
  - Notes passed, NPS (note-ons in the last second) and polyphony (note-ons minus the note-offs that release a held note of the same track, channel and key), from 1 ms tables built while the notes are paired at load time (on by default; not counted while all three are off).
  - Synth: messages sent (handed to the output), skipped and collapsed per second, the sender's lag and the skipping threshold.
  - Playback pipeline, to see what holds playback back: messages decoded per second, how busy the decoder (with "×N" while N threads decode) and sender threads are, how full the ring between them is (average and peak) and the output's cost per message. A busy decoder with an empty ring is the decoder's limit; a full ring with a busy sender is the output's, and a rising send cost means the synth is pushing back.
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
