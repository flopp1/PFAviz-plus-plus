//! (PFAviz++) A continuous Kestrel render on soundfonts already loaded: one synth and one live driver kept from
//! one block to the next, as a live host keeps them (Maestro's way), instead of a synth and a MIDI file per
//! piece. The host pushes each event at its output frame before asking for the audio that holds it, then asks
//! for the next frames in order; nothing runs on a thread of its own. Its voice cap can move while it plays
//! (Kestrel steals down to a lowered one). Kestrel only: Merlin renders whole files.
use anyhow::{bail, Context, Result};
use kestrel::gpu::GpuSynth;
use kestrel::midi::{sysex_to_events, Event};
use kestrel::{Backend, Config, Driver};

use crate::{config, decode, KbOptions, Loaded};

pub struct Stream {
    driver: Driver,
    synth: GpuSynth,
    /// One block of Kestrel's, and what of the last one rendered is not handed out yet.
    block: Vec<f32>,
    rest: Vec<f32>,
    rest_pos: usize,
    scratch: Vec<Event>,
}

impl Stream {
    pub fn open(loaded: &Loaded, opts: &KbOptions) -> Result<Stream> {
        let mut cfg = Config {
            filter_enabled: loaded.filter,
            gpu_backend: loaded.backend.clone(),
            gpu_adapter: loaded.card.clone(),
            coalesce: false,
            ..config(opts)?
        };
        // (limiter off = the raw mix, unclamped, as the pieces are: PFAviz++'s limiter runs over the joined audio)
        cfg.clamp_output = cfg.limiter;
        cfg.edo31 = false;
        if !loaded.bank.uses_lfo {
            cfg.lfo_enabled = false;
        }
        if !loaded.bank.uses_mod_env {
            cfg.mod_env_enabled = false;
        }
        if cfg.sample_rate != loaded.sample_rate {
            bail!("the soundfonts were loaded at {} Hz, not {}", loaded.sample_rate, cfg.sample_rate);
        }
        let synth = GpuSynth::on_shared(&cfg, loaded.bank.clone(), &loaded.gpu).context("the graphics card couldn't start Kestrel")?;
        let driver = Driver::live(&cfg, loaded.bank.clone())?;
        Ok(Stream { driver, synth, block: vec![0.0; cfg.block_samples()], rest: Vec::new(), rest_pos: 0, scratch: Vec::new() })
    }

    /// A message (status | d1 << 8 | d2 << 16) at `frame`; a note-on's loudness multiplied by `scale` (merged copies).
    pub fn push(&mut self, frame: u64, msg: u32, scale: f32) {
        if let Some(ev) = decode(msg) {
            self.driver.push_scaled(frame, ev, scale);
        }
    }

    /// A SysEx (from its F0) at `frame`: the tuning and part messages Kestrel reads.
    pub fn push_sysex(&mut self, frame: u64, data: &[u8]) {
        self.scratch.clear();
        sysex_to_events(data, 0, &mut self.scratch);
        for ev in self.scratch.drain(..) {
            if ev != Event::Other {
                self.driver.push(frame, ev);
            }
        }
    }

    /// The next `out.len() / 2` frames. Everything up to one Kestrel block past them must be pushed already.
    pub fn render(&mut self, out: &mut [f32]) -> Result<()> {
        let mut i = 0;
        while i < out.len() {
            if self.rest_pos < self.rest.len() {
                let n = (self.rest.len() - self.rest_pos).min(out.len() - i);
                out[i..i + n].copy_from_slice(&self.rest[self.rest_pos..self.rest_pos + n]);
                i += n;
                self.rest_pos += n;
                continue;
            }
            self.driver.render_live(&mut self.synth, &mut self.block)?;
            std::mem::swap(&mut self.rest, &mut self.block);
            self.rest_pos = 0;
            if self.block.len() != self.rest.len() {
                self.block.resize(self.rest.len(), 0.0);
            }
        }
        Ok(())
    }

    pub fn set_cap(&mut self, cap: u32) {
        self.driver.set_voice_cap(cap);
        self.synth.set_voice_cap(cap);
    }

    pub fn voices(&self) -> u64 {
        self.synth.stats().active_voices
    }
}
