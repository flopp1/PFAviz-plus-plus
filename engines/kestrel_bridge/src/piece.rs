//! One piece rendered ahead ("Rendered ahead" playback): a small MIDI file made into audio in memory, with Merlin
//! (Kestrel 1.4.0's per-key renderer: drops no note) or Kestrel, on soundfonts already loaded (`Loaded`: nothing
//! read again per piece). Runs on its own thread; can be cancelled (an edit throws a piece away) and says how far
//! it is. Settings as Kestrel's own `render` command has them, so a piece = Kestrel's render of that file.

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use anyhow::{bail, Result};
use kestrel::gpu::{GpuShared, GpuSynth};
use kestrel::merlin::progress::{Cancelled, MerlinProgress};
use kestrel::{Backend, Bank, Config, Driver};

use crate::{config, KbOptions, Loaded};

/// Which renderer makes a piece.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Engine {
    Kestrel,
    Merlin,
}

pub const RUNNING: u32 = 0;
pub const DONE: u32 = 1;
pub const FAILED: u32 = 2;
pub const CANCELLED: u32 = 3;

/// `kb_piece_status`'s answer. `size` = size_of as the caller knows it (fields added later go at the end).
#[repr(C)]
#[derive(Clone, Copy, Default, Debug)]
pub struct KbPieceStatus {
    pub size: u32,
    /// RUNNING / DONE / FAILED (kb_piece_error) / CANCELLED.
    pub state: u32,
    /// Stereo frames kept so far (the skipped ones not counted). Merlin hands its audio over at the end.
    pub frames: u64,
    /// 0..1 as well as it is known (Merlin: notes done of all; Kestrel: frames of `max_frames`, 0 when not given).
    pub progress: f32,
    /// Milliseconds since the start (to the end once finished).
    pub wall_ms: u32,
}

struct Shared {
    state: AtomicU32,
    frames: AtomicU64,
    progress: AtomicU32,
    wall_ms: AtomicU32,
    cancel: Arc<AtomicBool>,
    merlin: Mutex<Option<Arc<MerlinProgress>>>,
    audio: Mutex<Vec<f32>>,
    error: Mutex<Option<String>>,
    done: (Mutex<bool>, Condvar),
}

pub struct Piece {
    shared: Arc<Shared>,
    thread: Option<JoinHandle<()>>,
    /// The audio once done (moved out of `shared` so its pointer stays put).
    taken: Mutex<Option<Vec<f32>>>,
}

/// What a piece needs of a loaded set (the set may be freed while the piece runs).
struct Fonts {
    bank: Arc<Bank>,
    gpu: GpuShared,
    filter: bool,
    backend: Option<String>,
    card: Option<String>,
}

/// The piece's Kestrel settings: Spiderweb's options + what Kestrel's `render` command sets on top of a library's
/// defaults (coalescing on; Merlin = the per-key render on the card, filters on a 400-cent grid, notes follow a
/// moving bend), fitted to the bank as a render is.
fn piece_config(opts: &KbOptions, f: &Fonts, engine: Engine, midi: &Path) -> Result<Config> {
    let mut cfg = Config {
        filter_enabled: f.filter,
        gpu_backend: f.backend.clone(),
        gpu_adapter: f.card.clone(),
        coalesce: true,
        tempo_seed: opts.clock_seed,
        ..config(opts)?
    };
    // (limiter off = the raw mix, unclamped: Spiderweb's own limiter runs over the joined pieces; Kestrel's render
    // does the same for `--limiter off --format float32`)
    cfg.clamp_output = cfg.limiter;
    if engine == Engine::Merlin {
        cfg.merlin_key_major = true;
        cfg.merlin_key_major_gpu = true;
        cfg.merlin_key_parse = true;
        cfg.merlin_filter_cents = 400.0;
        cfg.merlin_km_warp = true;
    }
    if !f.bank.uses_lfo {
        cfg.lfo_enabled = false;
    }
    if !f.bank.uses_mod_env {
        cfg.mod_env_enabled = false;
    }
    if !cfg.edo31 && kestrel::midi::uses_extended_keys(midi).unwrap_or(false) {
        cfg.edo31 = true;
    }
    if engine == Engine::Merlin {
        // (what Merlin can't do yet: Spiderweb sends those pieces to Kestrel; the text says which)
        if cfg.edo31 {
            bail!("Merlin can't play 31-EDO yet");
        }
        if cfg.min_velocity > 1 {
            bail!("Merlin can't skip quiet notes yet (Velocity threshold)");
        }
        if f.bank.pool.is_empty() {
            bail!("Merlin plays one soundfont only for now");
        }
    }
    Ok(cfg)
}

impl Piece {
    /// Start making `midi` into audio: the first `skip` frames dropped (the piece starts early so notes already
    /// ringing sound right), then at most `max_frames` kept (0 = to the end, its ring-out included).
    pub fn start(loaded: &Loaded, midi: &Path, opts: &KbOptions, engine: Engine, skip: u64, max_frames: u64) -> Result<Piece> {
        Self::start_on(loaded, midi, false, opts, engine, skip, max_frames)
    }

    /// `start` with the MIDI file as bytes, never on disk: held under a name of its own (Kestrel's readers look
    /// there first) until the piece's thread ends.
    pub fn start_bytes(loaded: &Loaded, bytes: Arc<[u8]>, opts: &KbOptions, engine: Engine, skip: u64, max_frames: u64) -> Result<Piece> {
        static NEXT: AtomicU64 = AtomicU64::new(0);
        let name = PathBuf::from(format!("<spiderweb memory>/piece-{}.mid", NEXT.fetch_add(1, Ordering::Relaxed)));
        kestrel::midi::hold_in_memory(name.clone(), bytes);
        let got = Self::start_on(loaded, &name, true, opts, engine, skip, max_frames);
        if got.is_err() {
            kestrel::midi::let_go_in_memory(&name);
        }
        got
    }

    fn start_on(loaded: &Loaded, midi: &Path, held: bool, opts: &KbOptions, engine: Engine, skip: u64, max_frames: u64) -> Result<Piece> {
        let fonts = Fonts {
            bank: loaded.bank.clone(),
            gpu: loaded.gpu.clone(),
            filter: loaded.filter,
            backend: loaded.backend.clone(),
            card: loaded.card.clone(),
        };
        if opts.sample_rate != loaded.sample_rate {
            bail!("the soundfonts were loaded at {} Hz, not {}", loaded.sample_rate, opts.sample_rate);
        }
        let shared = Arc::new(Shared {
            state: AtomicU32::new(RUNNING),
            frames: AtomicU64::new(0),
            progress: AtomicU32::new(0f32.to_bits()),
            wall_ms: AtomicU32::new(0),
            cancel: Arc::new(AtomicBool::new(false)),
            merlin: Mutex::new(None),
            audio: Mutex::new(Vec::new()),
            error: Mutex::new(None),
            done: (Mutex::new(false), Condvar::new()),
        });
        let (s, opts, midi) = (shared.clone(), *opts, midi.to_path_buf());
        let thread = std::thread::Builder::new().name("kestrel piece".into()).spawn(move || {
            let t0 = Instant::now();
            let run = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                make(&s, &fonts, &midi, &opts, engine, skip, max_frames, t0)
            }));
            if held {  // (its bytes: freed as soon as it's made, before the audio is taken)
                kestrel::midi::let_go_in_memory(&midi);
            }
            let state = match run {
                Ok(Ok(())) if s.cancel.load(Ordering::Acquire) => CANCELLED,
                Ok(Ok(())) => DONE,
                Ok(Err(e)) if e.downcast_ref::<Cancelled>().is_some() || s.cancel.load(Ordering::Acquire) => CANCELLED,
                Ok(Err(e)) => {
                    *s.error.lock().unwrap_or_else(|e| e.into_inner()) = Some(format!("{e:#}"));
                    FAILED
                }
                Err(_) => {
                    *s.error.lock().unwrap_or_else(|e| e.into_inner()) = Some("Kestrel stopped unexpectedly".into());
                    FAILED
                }
            };
            if state == DONE {
                s.progress.store(1f32.to_bits(), Ordering::Release);
            }
            s.wall_ms.store(t0.elapsed().as_millis().min(u32::MAX as u128) as u32, Ordering::Release);
            s.state.store(state, Ordering::Release);
            let (lock, cv) = &s.done;
            *lock.lock().unwrap_or_else(|e| e.into_inner()) = true;
            cv.notify_all();
        })?;
        Ok(Piece { shared, thread: Some(thread), taken: Mutex::new(None) })
    }

    pub fn status(&self) -> KbPieceStatus {
        let s = &self.shared;
        let state = s.state.load(Ordering::Acquire);
        let mut progress = f32::from_bits(s.progress.load(Ordering::Acquire));
        if state == RUNNING {
            if let Some(m) = s.merlin.lock().unwrap_or_else(|e| e.into_inner()).as_ref() {
                let snap = m.snapshot();
                if snap.notes_total > 0 {
                    progress = (snap.notes_done as f64 / snap.notes_total as f64) as f32;
                }
            }
        }
        KbPieceStatus {
            size: std::mem::size_of::<KbPieceStatus>() as u32,
            state,
            frames: s.frames.load(Ordering::Acquire),
            progress: progress.clamp(0.0, 1.0),
            wall_ms: if state == RUNNING {
                0
            } else {
                s.wall_ms.load(Ordering::Acquire)
            },
        }
    }

    /// Stop it soon (Merlin: between its steps, usually well under a second).
    pub fn cancel(&self) {
        self.shared.cancel.store(true, Ordering::Release);
        if let Some(m) = self.shared.merlin.lock().unwrap_or_else(|e| e.into_inner()).as_ref() {
            m.cancel();
        }
    }

    /// Wait up to `timeout` for it to finish; its state.
    pub fn wait(&self, timeout: Duration) -> u32 {
        let (lock, cv) = &self.shared.done;
        let g = lock.lock().unwrap_or_else(|e| e.into_inner());
        let _ = cv.wait_timeout_while(g, timeout, |done| !*done);
        self.shared.state.load(Ordering::Acquire)
    }

    pub fn error(&self) -> Option<String> {
        self.shared.error.lock().unwrap_or_else(|e| e.into_inner()).clone()
    }

    /// The audio (stereo, interleaved, f32) once DONE; empty before. The slice stays valid while the piece lives.
    pub fn audio(&self) -> (*const f32, usize) {
        if self.shared.state.load(Ordering::Acquire) != DONE {
            return (std::ptr::null(), 0);
        }
        let mut t = self.taken.lock().unwrap_or_else(|e| e.into_inner());
        if t.is_none() {
            *t = Some(std::mem::take(&mut *self.shared.audio.lock().unwrap_or_else(|e| e.into_inner())));
        }
        let v = t.as_ref().unwrap();
        (v.as_ptr(), v.len() / 2)
    }
}

impl Drop for Piece {
    fn drop(&mut self) {
        self.cancel();
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

#[allow(clippy::too_many_arguments)]
fn make(s: &Shared, f: &Fonts, midi: &PathBuf, opts: &KbOptions, engine: Engine, skip: u64, max_frames: u64, t0: Instant) -> Result<()> {
    let cfg = piece_config(opts, f, engine, midi)?;
    let mut driver = Driver::open(&cfg, f.bank.clone(), midi)?;
    let mut backend: Box<dyn Backend> = match engine {
        Engine::Kestrel => Box::new(GpuSynth::on_shared(&cfg, f.bank.clone(), &f.gpu)?),
        Engine::Merlin => {
            let progress = MerlinProgress::new();
            let dir = kestrel::merlin::keystore::filing_dir(midi, &std::env::temp_dir(), cfg.merlin_disk_floor);
            *s.merlin.lock().unwrap_or_else(|e| e.into_inner()) = Some(progress.clone());
            if s.cancel.load(Ordering::Acquire) {
                progress.cancel();
            }
            Box::new(kestrel::merlin::keymajor::KeyMajor::with_progress(&cfg, f.bank.clone(), midi, &dir, progress)?)
        }
    };
    let mut block = vec![0.0f32; cfg.block_samples()];
    let (skip_s, max_s) = (skip as usize * 2, if max_frames == 0 { usize::MAX } else { max_frames as usize * 2 });
    let mut seen = 0usize; // samples rendered, skipped ones included
    let mut kept = 0usize;
    loop {
        if s.cancel.load(Ordering::Acquire) {
            return Err(Cancelled.into());
        }
        let more = driver.next_block(backend.as_mut(), &mut block)?;
        let lo = skip_s.saturating_sub(seen).min(block.len());
        let take = (block.len() - lo).min(max_s - kept);
        if take > 0 {
            s.audio.lock().unwrap_or_else(|e| e.into_inner()).extend_from_slice(&block[lo..lo + take]);
            kept += take;
            s.frames.store(kept as u64 / 2, Ordering::Release);
            if engine == Engine::Kestrel && max_frames > 0 {
                s.progress.store(((kept as f64 / max_s as f64) as f32).to_bits(), Ordering::Release);
            }
        }
        seen += block.len();
        if !more || kept >= max_s {
            break;
        }
    }
    let _ = t0;
    Ok(())
}
