# kestrel_bridge (PFAviz++'s copy)

The C connector PFAviz++ uses for audio rendered ahead with Kestrel and Merlin. It is
[Spiderweb](https://github.com/UnPrioritized/Spiderweb)'s `native/kestrel_bridge` (MIT, see `LICENSE`), over
[Kestrel](https://github.com/thtstickyboi/kestrel-midi) (MPL-2.0) as Spiderweb's fork pins it. PFAviz++ only uses
its "pieces" (`kb_piece_start_bytes` and the calls around it).

Changed here, marked `(PFAviz++)` in `src/lib.rs`:

- `KbOptions` carries Kestrel's performance settings at its end (gate resolution, workgroup size, render
  workgroups, voices per submission, voice building threads, voice sorting, compensated summation, unchecked
  shaders). A caller that passes the shorter struct gets Kestrel's own values, as before.
- `kb_take_warnings`: what the library logs as warnings (an SFZ's unsupported opcodes, samples that would not
  open, ...), collected from the first soundfont load on.
- `kb_stream_*` (`src/stream.rs`): a continuous Kestrel render on a loaded set, driven by the caller with no thread
  of its own: one synth and one live driver kept from block to block. Push messages at their frames
  (`kb_stream_push`, with a loudness factor per note-on for merged copies; `kb_stream_sysex`), then take the next
  frames (`kb_stream_render`); `kb_stream_set_cap` moves the voice cap while it plays.

## Building

```
cargo build --release
```

The result is `target/release/kestrel_bridge.dll`; put it in PFAviz++'s `vendor/engines/` to build it in. On
Windows it renders on DirectX 12 or Vulkan (Kestrel picks a discrete card first, and Vulkan before DirectX 12
on the same card); DirectX 12's shader compiler needs Visual Studio's ATL (the "C++ ATL" component), whose
`atlmfc\lib\x64` folder can be given to the linker with
`CARGO_ENCODED_RUSTFLAGS="-L"$'\x1f'"native=<that folder>"` when it is in another Visual Studio install than the
one cargo uses.
