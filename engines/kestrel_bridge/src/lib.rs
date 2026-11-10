//! Spiderweb's live connection to Kestrel (GPU SoundFont synth by thtstickyboi; live interface by mbms).
//!
//! Spiderweb (Python, ctypes) opens a synth with its soundfonts, pushes MIDI messages with the output frame each
//! should sound at, and BASS pulls the sound through `kb_streamproc` (a BASS STREAMPROC: no Python in the audio
//! path). A render thread keeps a few blocks ready ahead of the output. The note budget ("Lower it by itself")
//! lives here because only here is the GPU time of each block known.
//!
//! Frames: one shared count from 0 at `kb_open`. `rendered` = frames the render thread has made, `handed` =
//! Kestrel frames handed to the output. An event pushed for a frame already rendered sounds at the start of the
//! next block (Kestrel's live rule).

mod budget;
pub mod piece;
pub mod stream;
mod rank;

use std::collections::{HashMap, VecDeque};
use std::ffi::{c_char, c_void, CStr, CString};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::JoinHandle;
use std::time::Instant;

use anyhow::{bail, Context, Result};
use kestrel::gpu::{GpuShared, GpuSynth};
use kestrel::midi::Event;
use kestrel::config::AdmitRule;
use kestrel::limiter::LimiterMode;
use kestrel::{Backend, Bank, Config, Driver, Interpolation, StealRule};

use budget::Budget;

/// What `kb_open` is asked for. `size` = size_of::<KbOptions>() as the caller knows it (fields added later go at
/// the end; a shorter struct gets the defaults for them).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct KbOptions {
    pub size: u32,
    pub sample_rate: u32,
    pub block_frames: u32,
    /// Blocks kept ready ahead of the output (at least 2).
    pub ahead_blocks: u32,
    pub max_voices: u32,
    /// 1 = drop new notes (fair by track, quietest first) to stay in real time; 0 = never (it stutters instead).
    pub lower_by_itself: u32,
    /// Kestrel's limiter on the output: 0 = off, 1 = Brickwall (what "on" was), 2 = Balanced (Kestrel 1.4.0's render
    /// default; Spiderweb's too); and its ceiling in dBFS, -24..0 (0 = Kestrel's default; the user often uses -1).
    pub limiter: u32,
    pub limiter_ceiling_db: f32,
    /// Kestrel's DC blocker ahead of the limiter: 1 = on, at `dc_blocker_hz` (0 = Kestrel's 15 Hz). Off (Kestrel's
    /// own default), Shinki's stacked walls came out with a DC offset of -0.055 eating the limiter's room: every key
    /// ~5 dB under the user's render, which had one on (2026-10-27).
    pub dc_blocker: u32,
    pub dc_blocker_hz: f32,
    /// Settings → Sound's sound rows (13g, 2026-10-29), each Kestrel's own Config field at its default when 0 /
    /// out of range: interpolation 0 Nearest / 1 Linear / 2 Cubic; steal_rule 0 Oldest / 1 Drop new / 2 Quietest;
    /// release_floor_db = env_floor in dB (-100); min_velocity 0..127; admit_rule 0 Loudest / 1 Even;
    /// max_steal_percent (25); max_layers (16); filter / lfo / mod_env 1 = on.
    pub interpolation: u32,
    pub steal_rule: u32,
    pub release_floor_db: f32,
    pub min_velocity: u32,
    pub admit_rule: u32,
    pub max_steal_percent: u32,
    pub max_layers: u32,
    pub filter: u32,
    pub lfo: u32,
    pub mod_env: u32,
    /// (0.9.0) A piece's tempo clock starts with this many ticks carried over (Kestrel's Config::tempo_seed): where
    /// a clock that ran from the song's start has it, so the piece's notes fall on a whole render's frames. 0 = none.
    pub clock_seed: f64,
    /// (PFAviz++) Kestrel's performance settings, each Kestrel's own when 0: gate_frames, workgroup_size,
    /// max_render_workgroups, submit_voices, materialise_threads; and switches, 0 = Kestrel's own, 1 = off,
    /// 2 = on: sort_voices, kahan_reduce, unchecked_shaders.
    pub gate_frames: u32,
    pub workgroup_size: u32,
    pub max_render_workgroups: u32,
    pub submit_voices: u32,
    pub materialise_threads: u32,
    pub sort_voices: u32,
    pub kahan_reduce: u32,
    pub unchecked_shaders: u32,
}

impl Default for KbOptions {
    fn default() -> Self {
        KbOptions {
            size: std::mem::size_of::<KbOptions>() as u32,
            sample_rate: 48_000,
            block_frames: 512,
            // 6 x 512 = 64 ms of cushion: with 3 every swing of the note budget was a gap
            ahead_blocks: 6,
            max_voices: 1 << 20,
            lower_by_itself: 1,
            limiter: 1,
            limiter_ceiling_db: 0.0,
            dc_blocker: 1,
            dc_blocker_hz: 0.0,
            interpolation: 1,
            steal_rule: 2,
            release_floor_db: -100.0,
            min_velocity: 0,
            admit_rule: 0,
            max_steal_percent: 25,
            max_layers: 16,
            filter: 1,
            lfo: 1,
            mod_env: 1,
            clock_seed: 0.0,
            gate_frames: 0,
            workgroup_size: 0,
            max_render_workgroups: 0,
            submit_voices: 0,
            materialise_threads: 0,
            sort_voices: 0,
            kahan_reduce: 0,
            unchecked_shaders: 0,
        }
    }
}

/// Counters for the caller (all since `kb_open` unless said).
#[repr(C)]
#[derive(Clone, Copy, Default, Debug)]
pub struct KbStats {
    /// Frames rendered (the render thread's head).
    pub rendered: u64,
    /// Kestrel frames handed to the output.
    pub handed: u64,
    /// Frames of silence handed out because nothing was ready (each one = a stutter), after the first sound.
    pub silence: u64,
    /// Times the output found nothing ready, after the first sound.
    pub late: u64,
    pub voices: u64,
    /// Note-ons the budget dropped (with their note-offs).
    pub budget_dropped: u64,
    /// Note-ons Kestrel itself could not place, and voices it stole for room.
    pub kestrel_dropped: u64,
    pub stolen: u64,
    /// GPU + driver time of the last block, and the worst since the last `kb_stats` call, in microseconds.
    pub last_block_us: u32,
    pub worst_block_us: u32,
    /// Note-ons a block may take now; u32::MAX = no limit.
    pub allowance: u32,
    /// 1 = the render thread stopped on an error (`kb_error` says which); the output is silent.
    pub failed: u32,
    /// Of last_block_us: taking the block's messages, the budget, handing them to Kestrel.
    pub last_prep_us: u32,
    /// Voices Kestrel may sound now (Max voices unless "Lower it by itself" lowered it).
    pub voice_cap: u32,
    /// Times "Lower it by itself" cut the limits (it fell behind real time).
    pub cuts: u32,
    /// Different keys that wanted to start in a block / still started, summed over blocks.
    pub keys_wanted: u64,
    pub keys_kept: u64,
    /// Blocks chosen from the order made when sent (the quick way), and note-ons sent carrying merged copies.
    pub fast_blocks: u64,
    pub merged: u64,
    /// The loudness factors those carried: average and largest.
    pub merge_mean: f32,
    pub merge_max: f32,
}

#[derive(Clone, Copy)]
struct Pending {
    frame: u64,
    msg: u32,
    track: u16,
    /// A note-on's length in frames, paired with its note-off in the same push; LONG = its end wasn't in it
    /// (the budget keeps the longest start of a key: a held note beside spam on one key)
    len: u32,
    /// Ranked at push (rank.rs; the batch's `spans` say which blocks were): its place in its block's priority
    /// order, and the ranks of the notes its loudness goes to if it is dropped (`srep` its key's note on its
    /// frame, `krep` its key's tier-1 note).
    rank: u32,
    srep: u32,
    krep: u32,
    /// It starts on the same frame as its `srep` / `krep` note (its loudness adds as an amplitude there).
    s_same: bool,
    k_same: bool,
    /// Its length against its `srep` / `krep` note's (both capped at rank::HEARD): the share of that note's time
    /// it sounds, so a short copy adds its loudness only for as long as it lasted.
    s_ratio: f32,
    k_ratio: f32,
    /// A note-on standing for this many exact copies (kb_push_copies; 1 = itself).
    copies: u32,
}

impl Pending {
    fn new(frame: u64, msg: u32, track: u16) -> Self {
        Pending {
            frame,
            msg,
            track,
            len: LONG,
            rank: 0,
            srep: 0,
            krep: 0,
            s_same: false,
            k_same: false,
            s_ratio: 1.0,
            k_ratio: 1.0,
            copies: 1,
        }
    }
}

/// Measures (tests): nanoseconds summed in each step: 0 push copy, 1 push pairing, 2 push ranking, 3 render
/// choosing + handing to Kestrel, 4 Kestrel's render_live; 5 pushes, 6 blocks.
pub static PROFILE: [AtomicU64; 7] = [const { AtomicU64::new(0) }; 7];

fn profile_add(step: usize, since: Instant) -> Instant {
    let now = Instant::now();
    PROFILE[step].fetch_add((now - since).as_nanos() as u64, Ordering::Relaxed);
    now
}

/// `Pending::len` of a note whose end comes in a later push (or not a note-on).
pub(crate) const LONG: u32 = u32::MAX;

struct Ring {
    blocks: VecDeque<Vec<f32>>,
    /// Samples of the front block already handed out.
    pos: usize,
    spare: Vec<Vec<f32>>,
}

/// One push's messages, and where its ranked blocks are.
#[derive(Default)]
struct Batch {
    msgs: Vec<Pending>,
    spans: Vec<rank::Span>,
    /// (not the front batch) messages taken already: a later push due sooner (own shapes 0.2 s ahead behind the
    /// loaded MIDI's 1 s) is played on time, not after the one before it (hunt 2026-10-27: 0.8 s late)
    pos: usize,
    /// Pushed with copies (the loaded MIDI's feeder): `cut` takes these back from a frame on.
    song: bool,
}

#[derive(Default)]
struct Inbox {
    batches: VecDeque<Batch>,
    /// kb_cancel calls so far (the render thread drops what it took before the last one)
    cancels: u64,
    /// kb_cut: (number, frame) asked for; the render thread answers with (number, the frame it cut from).
    cut: Option<(u64, u64)>,
    cut_done: (u64, u64),
    cuts: u64,
}

struct Shared {
    inbox: Mutex<Inbox>,
    /// Played batches' memory, for the next pushes (fresh memory each push was most of push's copying: ~1.7 ms a
    /// Shinki block, the system handing out ~20 MB page by page).
    pool: Mutex<Vec<Batch>>,
    ring: Mutex<Ring>,
    /// Signalled when the output took a block (room to render) or on close.
    room: Condvar,
    stats: Mutex<KbStats>,
    error: Mutex<Option<String>>,
    stop: AtomicBool,
    lower: AtomicBool,
    /// The budget's note-ons a block now (u32::MAX = no limit), for push to know which blocks to rank.
    allow_now: AtomicU32,
    /// Tests (`Synth::fix_allowance`): note-ons a block fixed, 0 = learned; merging off.
    fixed_allow: AtomicU32,
    no_merge: AtomicBool,
    /// The song playing (`kb_song`: 0 = none, nothing remembered) and the output frame of its second 0 (f64 bits):
    /// each block's limits are remembered by its place in the song and taken again on the next play there.
    song: AtomicU32,
    origin: AtomicU64,
    ahead: usize,
    /// Worst block since the last kb_stats call.
    worst_us: AtomicU32,
}

pub struct Synth {
    shared: Arc<Shared>,
    /// push's scratch: note-ons waiting for their off, per channel + key (indices into the batch), and the ranker
    prep: Mutex<(Vec<VecDeque<u32>>, Vec<rank::Ranker>)>,
    thread: Option<JoinHandle<()>>,
    /// Measuring (KB_RECORD set): every sample handed to the output, saved as a WAV when closed.
    record: Option<Mutex<Vec<f32>>>,
    pub block_frames: u32,
    pub sample_rate: u32,
}

static LAST_ERROR: Mutex<Option<CString>> = Mutex::new(None);

fn set_error(text: &str) {
    *LAST_ERROR.lock().unwrap_or_else(|e| e.into_inner()) =
        Some(CString::new(text.replace('\0', " ")).unwrap_or_default());
}

/// Spiderweb's own status byte (undefined in MIDI) for "channel data1's part type is data2" (0 melodic, 1 / 2 the
/// drum maps), what a GS drum-part SysEx does: Spiderweb's own shapes keep channel 10 melodic (hunt 2026-10-28).
pub const PART_TYPE: u8 = 0xF5;

/// One MIDI message as Spiderweb packs it: status | data1 << 8 | data2 << 16 | port << 24.
pub fn decode(msg: u32) -> Option<Event> {
    let status = (msg & 0xFF) as u8;
    let d1 = ((msg >> 8) & 0x7F) as u8;
    let d2 = ((msg >> 16) & 0x7F) as u8;
    let port = (msg >> 24) as u8 % kestrel::midi::PORTS;
    if status == PART_TYPE {
        return Some(Event::DrumPart { ch: (port << 4) | (d1 & 0x0F), map: d2 });
    }
    let ch = (port << 4) | (status & 0x0F);
    Some(match status & 0xF0 {
        0x80 => Event::NoteOff { ch, key: d1 },
        0x90 if d2 == 0 => Event::NoteOff { ch, key: d1 },
        0x90 => Event::NoteOn { ch, key: d1, vel: d2 },
        0xB0 => Event::Cc { ch, num: d1, val: d2 },
        0xC0 => Event::Program { ch, val: d1 },
        0xE0 => Event::PitchBend { ch, val: (((d2 as i16) << 7) | d1 as i16) - 8192 },
        _ => return None,
    })
}

/// The settings a bank is loaded with: only the sample rate and the filter are Spiderweb's (Kestrel's defaults for
/// the rest), so a loaded bank serves every other setting. (The filter is decided per region as the soundfont is
/// read: Filter off = read again, 13g.)
fn bank_config(sample_rate: u32, filter: bool) -> Config {
    Config { sample_rate, filter_enabled: filter, ..Config::default() }
}

/// The backends Kestrel's own pick looks at here: on Windows every one but GL (Spiderweb never plays on GL, and
/// Kestrel's pick ranks it last). Looking at GL starts wgpu's "WGL Instance Thread", which ends a moment later
/// inside the NVIDIA driver while the Vulkan start runs there too: the driver's thread-end waits on its own lock
/// held by the Vulkan start, which waits on Windows' DLL loader lock held by the thread-end = hung for good
/// (Spiderweb PLAN E, 2026-11-04: 3 programs starting at once hung 9 of 30; dev/scratch/hang_start.py).
fn own_pick() -> Option<&'static str> {
    cfg!(windows).then_some("no-gl")
}

/// One soundfont read into memory, no graphics card (`kb_font_load`): Spiderweb keeps these by file, so a changed
/// soundfont list reads only the new file (reading is ~90 % of a start: 5 s for a 390 MB piano, 2026-10-29).
pub struct FontBank {
    bank: Arc<Bank>,
    sample_rate: u32,
    filter: bool,
}

impl FontBank {
    pub fn load(path: &std::path::Path, sample_rate: u32) -> Result<FontBank> {
        FontBank::load_with(path, sample_rate, true)
    }

    /// `load` with the soundfont's low-pass filters used (Kestrel's default) or left out.
    pub fn load_with(path: &std::path::Path, sample_rate: u32, filter: bool) -> Result<FontBank> {
        let bank = kestrel::load_bank(path, &bank_config(sample_rate, filter))?;
        Ok(FontBank { bank: Arc::new(bank), sample_rate, filter })
    }
}

/// Soundfonts stacked and their samples sent to the graphics card (`kb_load`): synths open on it in a moment
/// (`kb_open_on`), so a setting change never reads the soundfonts again.
pub struct Loaded {
    bank: Arc<Bank>,
    gpu: GpuShared,
    sample_rate: u32,
    /// (read with their filters: a synth on it keeps them whatever its options say, `KbOptions::filter`)
    filter: bool,
    /// The card it was asked for (pieces made with Merlin open their own device on the same one).
    backend: Option<String>,
    card: Option<String>,
}

impl Loaded {
    /// `fonts` top of Spiderweb's list first: each overrides the ones before it.
    pub fn new(fonts: &[&FontBank]) -> Result<Loaded> {
        Loaded::new_on(fonts, None, None)
    }

    /// `new` on one graphics card: `backend` ("vulkan", "dx12", ...) + a part of its name, as `kb_cards` lists
    /// them (None = Kestrel's own pick, the strongest card: on Windows without GL, see `own_pick`).
    pub fn new_on(fonts: &[&FontBank], backend: Option<String>, card: Option<String>) -> Result<Loaded> {
        match (backend, own_pick()) {
            (None, Some(no_gl)) => Loaded::new_exact(fonts, Some(no_gl.into()), card.clone())
                .or_else(|_| Loaded::new_exact(fonts, None, card)),  // (a PC with GL alone: as before)
            (backend, _) => Loaded::new_exact(fonts, backend, card),
        }
    }

    fn new_exact(fonts: &[&FontBank], backend: Option<String>, card: Option<String>) -> Result<Loaded> {
        let (first, rest) = fonts.split_first().context("no soundfont given")?;
        let sample_rate = first.sample_rate;
        if rest.iter().any(|f| f.sample_rate != sample_rate) {
            bail!("the soundfonts were read at different sample rates");
        }
        let filter = first.filter;
        if rest.iter().any(|f| f.filter != filter) {
            bail!("the soundfonts were read with and without their filters");
        }
        let cfg = Config { gpu_backend: backend.clone(), gpu_adapter: card.clone(), ..bank_config(sample_rate, filter) };
        // one soundfont = its own bank (no copy); more = copies stacked (the files' banks stay as they were)
        let bank = if rest.is_empty() {
            first.bank.clone()
        } else {
            let copies = rest.iter().map(|f| Ok(Bank::clone(&f.bank)));
            Arc::new(kestrel::session::stack(Bank::clone(&first.bank), rest.len(), copies, None, &cfg)?)
        };
        let phase = kestrel::phase::PhaseBank::prepare(&bank, &cfg.phase)?;
        // (the device + the samples only: each synth's own buffers come with its Max voices, as Kestrel's stems)
        let probe = Config { max_voices: 1, ..cfg };
        let gpu = GpuShared::new(&probe, &bank, &phase).context("the graphics card couldn't start Kestrel")?;
        drop(phase);
        let mut bank = bank;
        if !rest.is_empty() {
            // the stacked copy's samples live on the graphics card now: the PC copy freed (only the upload and
            // Kestrel's CPU renderer read them), so 2+ soundfonts take ~1x memory, not 2x (hunt 2026-10-29)
            if let Some(b) = Arc::get_mut(&mut bank) {
                b.pool = Vec::new();
            }
        }
        Ok(Loaded { bank, gpu, sample_rate, filter, backend, card })
    }

    /// Samples kept in the PC's memory for this set (0 for a stack: they're on the graphics card).
    pub fn pc_sample_bytes(&self) -> u64 {
        self.bank.pool_bytes()
    }

    /// The most Max voices this graphics card takes at `opts` (what a bigger open is refused with).
    pub fn voices_top(&self, opts: &KbOptions) -> Result<u32> {
        Ok(self.gpu.max_voices(&config(opts)?))
    }
}

/// A synth's Kestrel settings from Spiderweb's options.
fn config(opts: &KbOptions) -> Result<Config> {
    let block = opts.block_frames.clamp(64, 16384);
    let mut cfg = Config {
        sample_rate: opts.sample_rate,
        block_frames: block,
        max_voices: opts.max_voices.max(1),
        steal_fade_frames: Config::default().steal_fade_frames.min(block - 1),
        limiter: opts.limiter != 0,
        // (1 = Brickwall, as "on" was before 1.4.0; 2 = Balanced: a leveler ahead of the brickwall, Kestrel 1.4.0's
        // render default. Both keep 1.4.0's 20 ms brickwall hold: less distortion on low notes, ~1 dB quieter walls)
        limiter_mode: if opts.limiter == 2 { LimiterMode::Balanced } else { LimiterMode::Brickwall },
        dc_blocker: opts.dc_blocker != 0,
        dc_blocker_hz: if opts.dc_blocker_hz.is_finite() && opts.dc_blocker_hz > 0.0 {
            (opts.dc_blocker_hz as f64).clamp(1.0, 200.0)
        } else {
            Config::default().dc_blocker_hz
        },
        limiter_ceiling_db: if opts.limiter_ceiling_db.is_finite() {
            opts.limiter_ceiling_db.clamp(-24.0, 0.0) as _
        } else {
            0.0
        },
        interpolation: match opts.interpolation {
            0 => Interpolation::Nearest,
            2 => Interpolation::Cubic,
            _ => Interpolation::Linear,
        },
        steal_rule: match opts.steal_rule {
            0 => StealRule::Oldest,
            1 => StealRule::DropNew,
            _ => StealRule::Quietest,
        },
        env_floor: if opts.release_floor_db.is_finite() && opts.release_floor_db < 0.0 {
            10f32.powf(opts.release_floor_db.clamp(-160.0, -20.0) / 20.0)
        } else {
            Config::default().env_floor
        },
        min_velocity: opts.min_velocity.min(127) as u8,
        admit_rule: if opts.admit_rule == 1 { AdmitRule::Even } else { AdmitRule::Loudest },
        max_steal_percent: if (1..=100).contains(&opts.max_steal_percent) {
            opts.max_steal_percent
        } else {
            Config::default().max_steal_percent
        },
        max_layers: if (1..=64).contains(&opts.max_layers) { opts.max_layers } else { Config::default().max_layers },
        filter_enabled: opts.filter != 0,
        lfo_enabled: opts.lfo != 0,
        mod_env_enabled: opts.mod_env != 0,
        ..Config::default()
    };
    // (PFAviz++: the performance settings)
    if opts.gate_frames > 0 {
        cfg.gate_frames = opts.gate_frames;
    }
    if opts.workgroup_size > 0 {
        cfg.workgroup_size = opts.workgroup_size;
    }
    if opts.max_render_workgroups > 0 {
        cfg.max_render_workgroups = opts.max_render_workgroups;
    }
    if opts.submit_voices > 0 {
        cfg.submit_voices = opts.submit_voices;
    }
    if opts.materialise_threads > 0 {
        cfg.materialise_threads = opts.materialise_threads as usize;
    }
    let switch = |v: u32, d: bool| match v {
        1 => false,
        2 => true,
        _ => d,
    };
    cfg.sort_voices = switch(opts.sort_voices, cfg.sort_voices);
    cfg.kahan_reduce = switch(opts.kahan_reduce, cfg.kahan_reduce);
    cfg.unchecked_shaders = switch(opts.unchecked_shaders, cfg.unchecked_shaders);
    cfg.validate()?;
    Ok(cfg)
}

/// (PFAviz++) The library's warnings (an SFZ's unsupported opcodes, a soundfont's odd parts, ...), kept until
/// kb_take_warnings takes them.
struct Collect;
static WARNINGS: Mutex<Vec<String>> = Mutex::new(Vec::new());
static COLLECT: Collect = Collect;
impl log::Log for Collect {
    fn enabled(&self, m: &log::Metadata) -> bool {
        m.level() <= log::Level::Warn
    }
    fn log(&self, r: &log::Record) {
        if self.enabled(r.metadata()) {
            let mut w = WARNINGS.lock().unwrap_or_else(|e| e.into_inner());
            if w.len() < 200 {
                w.push(format!("{}", r.args()));
            }
        }
    }
    fn flush(&self) {}
}
fn collect_warnings() {
    static ONCE: std::sync::Once = std::sync::Once::new();
    ONCE.call_once(|| {
        if log::set_logger(&COLLECT).is_ok() {
            log::set_max_level(log::LevelFilter::Warn);
        }
    });
}
static TAKEN: Mutex<Option<CString>> = Mutex::new(None);

/// (PFAviz++) The warnings collected since the last call, one a line (UTF-8; empty when none), valid until the next call.
#[no_mangle]
pub extern "C" fn kb_take_warnings() -> *const c_char {
    collect_warnings();
    let text = std::mem::take(&mut *WARNINGS.lock().unwrap_or_else(|e| e.into_inner())).join("\n");
    let mut t = TAKEN.lock().unwrap_or_else(|e| e.into_inner());
    *t = CString::new(text.replace('\0', " ")).ok();
    t.as_ref().map_or(std::ptr::null(), |c| c.as_ptr())
}

impl Synth {
    /// Read, stack, start the graphics card and open, all in one (`Loaded` + `open_on`).
    pub fn open(fonts: &[PathBuf], opts: &KbOptions) -> Result<Synth> {
        if fonts.is_empty() {
            bail!("no soundfont given");
        }
        let banks = fonts
            .iter()
            .map(|f| FontBank::load_with(f, opts.sample_rate, opts.filter != 0))
            .collect::<Result<Vec<_>>>()?;
        let loaded = Loaded::new(&banks.iter().collect::<Vec<_>>())?;
        drop(banks);
        Synth::open_on(&loaded, opts)
    }

    /// A synth on soundfonts already loaded: only its own buffers + render thread (~20 ms).
    pub fn open_on(loaded: &Loaded, opts: &KbOptions) -> Result<Synth> {
        let cfg = Config { filter_enabled: loaded.filter, ..config(opts)? };
        if cfg.sample_rate != loaded.sample_rate {
            bail!("the soundfonts were loaded at {} Hz, not {}", loaded.sample_rate, cfg.sample_rate);
        }
        let bank = loaded.bank.clone();
        let synth = GpuSynth::on_shared(&cfg, bank.clone(), &loaded.gpu)
            .context("the graphics card couldn't start Kestrel")?;
        let driver = Driver::live(&cfg, bank)?;
        let ahead = opts.ahead_blocks.max(2) as usize;
        let shared = Arc::new(Shared {
            inbox: Mutex::new(Inbox::default()),
            pool: Mutex::new(Vec::new()),
            ring: Mutex::new(Ring { blocks: VecDeque::new(), pos: 0, spare: Vec::new() }),
            room: Condvar::new(),
            stats: Mutex::new(KbStats { allowance: u32::MAX, ..Default::default() }),
            error: Mutex::new(None),
            stop: AtomicBool::new(false),
            lower: AtomicBool::new(opts.lower_by_itself != 0),
            allow_now: AtomicU32::new(u32::MAX),
            fixed_allow: AtomicU32::new(0),
            no_merge: AtomicBool::new(false),
            song: AtomicU32::new(0),
            origin: AtomicU64::new(0),
            ahead,
            worst_us: AtomicU32::new(0),
        });
        Synth::start(shared, driver, synth, &cfg)
    }

    fn start(shared: Arc<Shared>, driver: Driver, synth: GpuSynth, cfg: &Config) -> Result<Synth> {
        let block = cfg.block_frames;
        let s = shared.clone();
        let rate = cfg.sample_rate;
        let max_voices = cfg.max_voices;
        let thread = std::thread::Builder::new()
            .name("kestrel render".into())
            .spawn(move || {
                let s2 = s.clone();
                let run = std::panic::catch_unwind(std::panic::AssertUnwindSafe(move || {
                    render_loop(&s2, driver, synth, block, rate, max_voices)
                }));
                let err = match run {
                    Ok(Ok(())) => None,
                    Ok(Err(e)) => Some(format!("{e:#}")),
                    Err(_) => Some("Kestrel stopped unexpectedly".to_string()),
                };
                if let Some(e) = err {
                    *s.error.lock().unwrap_or_else(|e| e.into_inner()) = Some(e);
                    s.stats.lock().unwrap_or_else(|e| e.into_inner()).failed = 1;
                }
            })?;
        Ok(Synth {
            shared,
            prep: Mutex::new((vec![VecDeque::new(); 256 * 128], rankers())),
            thread: Some(thread),
            record: std::env::var_os("KB_RECORD").map(|_| Mutex::new(Vec::new())),
            block_frames: block,
            sample_rate: rate,
        })
    }

    /// Queue messages (frames non-decreasing is best; an earlier one is played with the one queued before it).
    /// Made into one batch first: the render thread is never kept waiting while a big push copies.
    pub fn push(&self, frames: &[u64], msgs: &[u32], tracks: &[u16]) {
        self.push_len(frames, msgs, tracks, None);
    }

    /// `push` with each note-on's length in frames (`lens[i]`, LONG = not known; other messages: anything). The
    /// caller knows them (Spiderweb does): paired inside one push the connector only knows notes ending in it, and
    /// pushed a block at a time nearly every note looked long, so loud spam beat held notes again.
    pub fn push_len(&self, frames: &[u64], msgs: &[u32], tracks: &[u16], lens: Option<&[u32]>) {
        self.push_full(frames, msgs, tracks, lens, None);
    }

    /// `push_len` with each note-on standing for `copies[i]` exact copies (kb_push_copies).
    pub fn push_full(&self, frames: &[u64], msgs: &[u32], tracks: &[u16], lens: Option<&[u32]>, copies: Option<&[u32]>) {
        let mut n = frames.len().min(msgs.len()).min(tracks.len());
        if let Some(l) = lens {
            n = n.min(l.len());
        }
        if let Some(c) = copies {
            n = n.min(c.len());
        }
        if n == 0 {
            return;
        }
        let t = Instant::now();
        PROFILE[5].fetch_add(1, Ordering::Relaxed);
        let mut whole = self.shared.pool.lock().unwrap().pop().unwrap_or_default();
        whole.spans.clear();
        whole.pos = 0;
        whole.song = copies.is_some();
        let batch = &mut whole.msgs;
        batch.clear();
        // note lengths given, or paired below inside this push (FIFO per channel + key)
        match lens {
            Some(l) => batch.extend((0..n).map(|i| Pending { len: l[i], ..Pending::new(frames[i], msgs[i], tracks[i]) })),
            None => batch.extend((0..n).map(|i| Pending::new(frames[i], msgs[i], tracks[i]))),
        }
        if let Some(c) = copies {
            for (p, &c) in batch.iter_mut().zip(c) {
                p.copies = c.max(1);
            }
        }
        let t = profile_add(0, t);
        // then each busy block's priority order: worked out here on the caller's thread, not the render thread's
        {
            let mut prep = self.prep.lock().unwrap();
            let (open, ranker) = &mut *prep;
            for i in (0..n).take_while(|_| lens.is_none()) {
                let m = batch[i].msg;
                let status = m & 0xF0;
                if status != 0x90 && status != 0x80 {
                    continue;
                }
                let slot = ((((m >> 24) & 0x0F) << 4 | (m & 0x0F)) * 128 + ((m >> 8) & 0x7F)) as usize;
                if status == 0x90 && (m >> 16) & 0x7F != 0 {
                    open[slot].push_back(i as u32);
                } else if let Some(j) = open[slot].pop_front() {
                    let j = j as usize;
                    batch[j].len = (batch[i].frame.saturating_sub(batch[j].frame)).min(LONG as u64 - 1) as u32;
                }
            }
            for i in (0..n).take_while(|_| lens.is_none()) {
                let m = batch[i].msg;
                if m & 0xF0 == 0x90 {
                    let slot = ((((m >> 24) & 0x0F) << 4 | (m & 0x0F)) * 128 + ((m >> 8) & 0x7F)) as usize;
                    open[slot].clear();
                }
            }
            // only blocks that may go over the allowance are worth ranking (it may halve before they play; and
            // past RANK_ALWAYS note-ons a block is ranked anyway: a cut must not find a wall unranked)
            let t = profile_add(1, t);
            let allow = self.shared.allow_now.load(Ordering::Relaxed) as usize;
            let over = (allow / 2).min(rank::RANK_ALWAYS);
            let bf = self.block_frames as u64;
            // a big push: its blocks ranked side by side, a share of whole blocks each (Spiderweb's feeder playing
            // Shinki's dense part: ~0.8 s of one core a second of song, it fell behind)
            let parts = if n >= SIDE_BY_SIDE { ranker.len() } else { 1 };
            let mut cuts = vec![0usize];
            for k in 1..parts {
                let mut c = (n * k / parts).max(*cuts.last().unwrap());
                while c < n && c > 0 && batch[c].frame / bf == batch[c - 1].frame / bf {
                    c += 1;
                }
                cuts.push(c);
            }
            cuts.push(n);
            if parts == 1 {
                ranker[0].rank(batch, bf, over, &mut whole.spans);
            } else {
                let mut found: Vec<Vec<rank::Span>> = vec![Vec::new(); parts];
                std::thread::scope(|sc| {
                    let mut rest: &mut [Pending] = batch;
                    let mut at = 0;
                    for ((r, f), w) in ranker.iter_mut().zip(found.iter_mut()).zip(cuts.windows(2)) {
                        let (mine, more) = std::mem::take(&mut rest).split_at_mut(w[1] - at);
                        rest = more;
                        at = w[1];
                        sc.spawn(move || r.rank(mine, bf, over, f));
                    }
                });
                for (f, &start) in found.iter().zip(&cuts) {
                    whole.spans.extend(f.iter().map(|s| rank::Span { start: s.start + start as u32, ..*s }));
                }
            }
            profile_add(2, t);
        }
        self.shared.inbox.lock().unwrap().batches.push_back(whole);
    }

    /// The loaded MIDI's messages (pushed with copies) from `frame` on taken back, never one already made into
    /// sound: returns the frame they were taken back from (`frame`, or the first block not made yet), None when the
    /// render thread didn't answer within a second. The tempo changed while playing: Spiderweb pushes them again
    /// at the new speed (they were pushed a second ahead).
    pub fn cut(&self, frame: u64) -> Option<u64> {
        let id = {
            let mut inbox = self.shared.inbox.lock().unwrap();
            inbox.cuts += 1;
            inbox.cut = Some((inbox.cuts, frame));
            inbox.cuts
        };
        let t = Instant::now();
        while t.elapsed().as_secs_f64() < 1.0 {
            let (n, at) = self.shared.inbox.lock().unwrap().cut_done;
            if n == id {
                return Some(at);
            }
            std::thread::sleep(std::time::Duration::from_micros(500));
        }
        None
    }

    /// Forget every message not rendered yet (sounding notes go on: send note-offs after this).
    pub fn cancel(&self) {
        let mut inbox = self.shared.inbox.lock().unwrap();
        inbox.batches.clear();
        inbox.cancels += 1;
    }

    /// Tests: note-ons a block fixed at `allow` (0 = learned again), the voice cap at Max voices: the choice and
    /// merging measured with no real-time timing in it.
    pub fn fix_allowance(&self, allow: u32, merge: bool) {
        self.shared.no_merge.store(!merge, Ordering::Relaxed);
        self.shared.fixed_allow.store(allow, Ordering::Relaxed);
        self.shared.allow_now.store(if allow == 0 { u32::MAX } else { allow }, Ordering::Relaxed);
    }

    /// The song being pushed (`song` 0 = none) and the output frame its second 0 falls on: the note budget plays a
    /// place in the song with the limits it had there the first time (the same notes kept, the same sound every
    /// play). Another song = the remembered limits go.
    pub fn set_song(&self, origin: f64, song: u32) {
        self.shared.origin.store(origin.to_bits(), Ordering::Relaxed);
        self.shared.song.store(song, Ordering::Relaxed);
    }

    pub fn set_lower(&self, on: bool) {
        self.shared.lower.store(on, Ordering::Relaxed);
    }

    pub fn stats(&self) -> KbStats {
        let mut st = *self.shared.stats.lock().unwrap();
        st.worst_block_us = self.shared.worst_us.swap(0, Ordering::Relaxed);
        st
    }

    pub fn error(&self) -> Option<String> {
        self.shared.error.lock().unwrap().clone()
    }

    /// Hand out interleaved stereo samples: what's ready, silence for the rest.
    pub fn fill(&self, out: &mut [f32]) {
        let mut handed = 0u64;
        let mut silent = 0u64;
        {
            let mut guard = self.shared.ring.lock().unwrap();
            let r = &mut *guard;
            let mut i = 0;
            while i < out.len() {
                let Some(front) = r.blocks.front() else {
                    out[i..].fill(0.0);
                    silent = ((out.len() - i) / 2) as u64;
                    break;
                };
                let n = (front.len() - r.pos).min(out.len() - i);
                out[i..i + n].copy_from_slice(&front[r.pos..r.pos + n]);
                i += n;
                r.pos += n;
                handed += (n / 2) as u64;
                if r.pos == front.len() {
                    let b = r.blocks.pop_front().unwrap();
                    r.spare.push(b);
                    r.pos = 0;
                }
            }
        }
        self.shared.room.notify_one();
        if let Some(rec) = self.record.as_ref() {
            rec.lock().unwrap().extend_from_slice(out);
        }
        let mut st = self.shared.stats.lock().unwrap();
        // (silence before the first block was ready isn't a stutter)
        let started = st.handed > 0;
        st.handed += handed;
        if silent > 0 && started {
            st.silence += silent;
            st.late += 1;
        }
    }

    /// Wait until the render thread has `blocks` blocks ready (tests: a known starting point).
    pub fn wait_ready(&self, blocks: usize) {
        let blocks = blocks.min(self.shared.ahead);
        loop {
            if self.shared.ring.lock().unwrap().blocks.len() >= blocks || self.error().is_some() {
                return;
            }
            std::thread::sleep(std::time::Duration::from_millis(1));
        }
    }
}

impl Drop for Synth {
    fn drop(&mut self) {
        self.shared.stop.store(true, Ordering::Relaxed);
        self.shared.room.notify_all();
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
        // (measuring: KB_RECORD = a WAV file, everything handed to the output from kb_open on, 16-bit, saved here)
        if let (Some(rec), Some(path)) = (self.record.as_ref(), std::env::var_os("KB_RECORD")) {
            let rec = rec.lock().unwrap_or_else(|e| e.into_inner());
            let pcm: Vec<u8> = rec.iter().flat_map(|x| ((x.clamp(-1.0, 1.0) * 32767.0) as i16).to_le_bytes()).collect();
            let rate = self.sample_rate;
            let mut f = Vec::with_capacity(44 + pcm.len());
            f.extend_from_slice(b"RIFF");
            f.extend_from_slice(&(36 + pcm.len() as u32).to_le_bytes());
            f.extend_from_slice(b"WAVEfmt ");
            for v in [16u32.to_le_bytes(), [1, 0, 2, 0], rate.to_le_bytes(), (rate * 4).to_le_bytes(), [4, 0, 16, 0]] {
                f.extend_from_slice(&v);
            }
            f.extend_from_slice(b"data");
            f.extend_from_slice(&(pcm.len() as u32).to_le_bytes());
            f.extend_from_slice(&pcm);
            let _ = std::fs::write(path, f);
        }
    }
}

/// A remembered song block's limits go up by PLAN_RISE after a play where it left notes out with time to spare
/// (made in under PLAN_ROOM of its real-time length), never past the limits it had the first time: climbing past
/// them changed the sound every play (2026-11-05: a dense wall's limits rose ~6 % a play, play 5 vs 1 = 9 % same).
const PLAN_ROOM: f64 = 0.6;
const PLAN_RISE: f32 = 1.1;
/// Messages other batches may have due in a block for its front batch to still be read the fast way (`apart`).
const FEW_OTHERS: usize = 4096;

fn render_loop(s: &Shared, mut driver: Driver, mut synth: GpuSynth, block: u32, rate: u32, max_voices: u32) -> Result<()> {
    let samples = block as usize * 2;
    let block_us = block as f64 * 1e6 / rate as f64;
    let mut budget = Budget::new(max_voices);
    let mut cap = max_voices;
    let mut taken: Vec<Pending> = Vec::new();
    // (a block's kept messages when its few others are chosen apart: frame, message, loudness factor)
    let mut kept_a: Vec<(u64, u32, f32)> = Vec::new();
    let mut kept_b: Vec<(u64, u32, f32)> = Vec::new();
    // batches taken from the inbox, not due yet (front batch read from `at`)
    let mut future: VecDeque<Batch> = VecDeque::new();
    let mut at = 0usize;
    // (the front batch's next span to look at)
    let mut span = 0usize;
    let mut cancels = 0u64;
    // each song block's limits as first played (note-ons, voices): played again with them (the same sound every
    // play, 2026-10-28): (note-ons, voices) now + the first play's (note-ons, voices), the most a rise goes back to
    let mut plan: HashMap<i64, (f32, f32, f32, f32)> = HashMap::new();
    let mut plan_song = 0u32;
    // (measuring: KB_NOPLAN = nothing remembered, as before)
    let plan_on = std::env::var_os("KB_NOPLAN").is_none();
    // (measuring: KB_LOG = a file path, one line a block)
    let mut log = std::env::var_os("KB_LOG").and_then(|p| std::fs::File::create(p).ok()).map(std::io::BufWriter::new);
    loop {
        let (mut out, ready) = {
            let mut r = s.ring.lock().unwrap();
            while r.blocks.len() >= s.ahead && !s.stop.load(Ordering::Relaxed) {
                r = s.room.wait(r).unwrap();
            }
            if s.stop.load(Ordering::Relaxed) {
                return Ok(());
            }
            let ready = r.blocks.len();
            (r.spare.pop().unwrap_or_else(|| vec![0.0; samples]), ready)
        };
        let t = Instant::now();
        let end = driver.next_frame() + block as u64;
        {
            let mut inbox = s.inbox.lock().unwrap();
            if inbox.cancels != cancels {
                cancels = inbox.cancels;
                for b in future.drain(..) {
                    recycle(s, b);
                }
                at = 0;
                span = 0;
                budget.forget();
                budget.restart();
            }
            future.extend(inbox.batches.drain(..));
            if let Some((id, frame)) = inbox.cut.take() {
                // (blocks before this one are made: never taken back)
                let from = frame.max(end - block as u64);
                for (i, b) in future.iter_mut().enumerate() {
                    let start = if i == 0 { at } else { b.pos }.min(b.msgs.len());
                    if !b.song {
                        continue;
                    }
                    if let Some(k) = b.msgs[start..].iter().position(|p| p.frame >= from) {
                        b.msgs.truncate(start + k);
                        b.spans.retain(|g| (g.start + g.len) as usize <= start + k);
                    }
                }
                inbox.cut_done = (id, from);
            }
        }
        let song = s.song.load(Ordering::Relaxed);
        if song != 0 && song != plan_song {  // (0 = between plays / own shapes alone: kept for the next play)
            plan.clear();
            plan_song = song;
        }
        // (the block's place in the song, in blocks)
        let place = (song != 0 && plan_on && s.lower.load(Ordering::Relaxed) && s.fixed_allow.load(Ordering::Relaxed) == 0).then(|| {
            (((end - block as u64) as f64 - f64::from_bits(s.origin.load(Ordering::Relaxed))) / block as f64).round() as i64
        });
        let had = place.and_then(|p| plan.get(&p).copied());
        if let Some((a, c, _, _)) = had {
            budget.follow(a, c);
            if budget.voice_cap() != cap {
                cap = budget.voice_cap();
                driver.set_voice_cap(cap);
                synth.set_voice_cap(cap);
            }
        }
        let used = budget.limits();
        let cuts = budget.cuts;
        // (a batch's frames rise, mostly: one earlier than the one before it is played with that one, as Kestrel's
        // own queue does; so a batch is read in order up to the first frame at or past the block's end)
        let lower = s.lower.load(Ordering::Relaxed);
        let mut push = |p: &Pending, scale: f32| {
            if let Some(ev) = decode(p.msg) {
                // (our Kestrel change: a note-on carrying the copies merged into it)
                driver.push_scaled(p.frame, ev, scale);
            }
        };
        // the whole block inside the front batch (Spiderweb pushes whole blocks): read where it is. Copying
        // ~370,000 messages a block (Shinki) into `taken` was most of the render thread's ~3.7 ms of preparing.
        // (a batch read to its end: gone first, so a block starting the next one is read in place too)
        while future.front().is_some_and(|b| at >= b.msgs.len()) {
            let b = future.pop_front().unwrap();
            recycle(s, b);
            at = future.front().map_or(0, |b| b.pos);
            span = 0;
        }
        // (a later batch with messages due in this block too: all read the general way below; unless they're few
        // (own shapes' notes beside a loaded MIDI's busy block): then the busy block keeps its fast way, the few are
        // chosen apart, both put together by frame. A note every 20 ms beside Shinki sent 394 of 690 blocks the slow
        // way and the note limit fell from ~7,000 a block to ~200, C6 2026-10-28)
        let mut few = 0usize;
        let mut others = false;
        for b in future.iter().skip(1) {
            let mut k = b.pos;
            while k < b.msgs.len() && b.msgs[k].frame < end && few <= FEW_OTHERS {
                few += 1;
                k += 1;
            }
            others |= k > b.pos;
        }
        let apart = others && few <= FEW_OTHERS;
        let alone = !others || apart;
        // A ranked block starting here and ending before `end` is the whole block: no search through it.
        let ranked = future.front().filter(|_| alone).and_then(|b| {
            while span < b.spans.len() && (b.spans[span].start as usize) < at {
                span += 1;
            }
            let g = b.spans.get(span).filter(|g| g.start as usize == at)?;
            let stop = at + g.len as usize;
            (b.msgs[at].frame < end && b.msgs.get(stop).map_or(true, |p| p.frame >= end)).then_some((stop, g.ons))
        });
        let inside = ranked.map(|r| r.0).or_else(|| {
            future.front().filter(|_| alone).and_then(|b| b.msgs[at..].iter().position(|p| p.frame >= end).map(|j| at + j))
        });
        let n_msgs;
        let dropped = if let Some(j) = inside.filter(|_| apart) {
            n_msgs = j - at + few;
            kept_a.clear();
            kept_b.clear();
            let mut d = budget.filter(&future.front().unwrap().msgs[at..j], ranked.map(|r| r.1 as usize), lower,
                                      |p: &Pending, sc: f32| kept_a.push((p.frame, p.msg, sc)));
            let wanted = budget.wanted();
            for b in future.iter_mut().skip(1) {
                while b.pos < b.msgs.len() && b.msgs[b.pos].frame < end {
                    taken.push(b.msgs[b.pos]);
                    b.pos += 1;
                }
            }
            d += budget.filter(&taken, None, lower, |p: &Pending, sc: f32| kept_b.push((p.frame, p.msg, sc)));
            budget.add_wanted(wanted);
            taken.clear();
            // (by frame, each one's own order kept)
            let (mut a, mut b) = (0, 0);
            while a < kept_a.len() || b < kept_b.len() {
                let (f, m, sc) = if b >= kept_b.len() || (a < kept_a.len() && kept_a[a].0 <= kept_b[b].0) {
                    a += 1;
                    kept_a[a - 1]
                } else {
                    b += 1;
                    kept_b[b - 1]
                };
                if let Some(ev) = decode(m) {
                    driver.push_scaled(f, ev, sc);
                }
            }
            at = j;
            d
        } else if let Some(j) = inside {
            n_msgs = j - at;
            let d = budget.filter(&future.front().unwrap().msgs[at..j], ranked.map(|r| r.1 as usize), lower, &mut push);
            at = j;
            d
        } else {
            // every batch's messages due in this block, each read from where it is; several batches = put together
            // by frame, each one's own order kept (sorted by its running highest frame: as Kestrel plays a batch)
            let mut keys: Vec<u64> = Vec::new();
            for (i, batch) in future.iter_mut().enumerate() {
                let mut k = if i == 0 { at } else { batch.pos };
                let mut high = 0u64;
                while k < batch.msgs.len() && batch.msgs[k].frame < end {
                    taken.push(batch.msgs[k]);
                    if others {
                        high = high.max(batch.msgs[k].frame);
                        keys.push(high);
                    }
                    k += 1;
                }
                if i == 0 {
                    at = k;
                } else {
                    batch.pos = k;
                }
            }
            if others {
                let mut order: Vec<usize> = (0..taken.len()).collect();
                order.sort_by_key(|&i| keys[i]);
                taken = order.iter().map(|&i| taken[i]).collect();
            }
            // (batches read to their end go; the front one at the next block)
            let mut i = 1;
            while i < future.len() {
                if future[i].pos >= future[i].msgs.len() {
                    let b = future.remove(i).unwrap();
                    recycle(s, b);
                } else {
                    i += 1;
                }
            }
            n_msgs = taken.len();
            let d = budget.filter(&taken, None, lower, &mut push);
            taken.clear();
            d
        };
        let prep = t.elapsed();
        let tp = profile_add(3, t);
        PROFILE[6].fetch_add(1, Ordering::Relaxed);
        driver.render_live(&mut synth, &mut out)?;
        profile_add(4, tp);
        let us = t.elapsed().as_secs_f64() * 1e6;
        let bs = synth.stats();
        budget.fix(s.fixed_allow.load(Ordering::Relaxed), !s.no_merge.load(Ordering::Relaxed));
        budget.measured(us / block_us, ready, s.ahead, bs.active_voices, lower);
        if let Some(p) = place.filter(|_| budget.wanted() > 0) {
            match had {
                // (only limits that kept up: a play's first busy block has none yet and took ~120 ms in Shinki;
                // remembered, every play there began with that gap)
                None if us <= block_us => {
                    plan.insert(p, (used.0, used.1, used.0, used.1));
                }
                None if budget.cuts != cuts => {
                    let (a, c) = budget.limits();
                    plan.insert(p, (a, c, a, c));
                }
                // (too much for this PC now: lower here from now on, never higher)
                Some((a, c, a0, c0)) if budget.cuts != cuts => {
                    let (a2, c2) = budget.limits();
                    plan.insert(p, (a.min(a2), c.min(c2), a0, c0));
                }
                // (lowered after a slow moment, with time to spare now: a little more there next play, back up to the
                // first play's limits at most. One slow moment (the card waking, Windows busy) kept a spot thinned for
                // the whole session, hunt 2026-10-28)
                Some((a, c, a0, c0)) if dropped > 0 && us < block_us * PLAN_ROOM && (a < a0 || c < c0) => {
                    plan.insert(p, ((a * PLAN_RISE + 1.0).min(a0), (c * PLAN_RISE).min(c0).min(max_voices as f32),
                                    a0, c0));
                }
                _ => {}
            }
        }
        if budget.voice_cap() != cap {
            // (our change to Kestrel: a voice cap that moves while it plays; over it = quietest stolen)
            cap = budget.voice_cap();
            driver.set_voice_cap(cap);
            synth.set_voice_cap(cap);
        }
        s.worst_us.fetch_max(us as u32, Ordering::Relaxed);
        if let Some(f) = log.as_mut() {
            use std::io::Write;
            let _ = writeln!(f, "{} place {place:?} msgs {n_msgs} ranked {:?} dropped {dropped} allow {} cap {cap} voices {} ready {ready} \
                prep {:.2} ms block {:.2} ms", end - block as u64, ranked.map(|r| r.1), used.0,
                bs.active_voices, prep.as_secs_f64() * 1e3, us / 1e3);
        }
        {
            let mut st = s.stats.lock().unwrap();
            st.rendered = driver.next_frame();
            st.voices = bs.active_voices;
            st.kestrel_dropped = bs.dropped;
            st.stolen = bs.stolen;
            st.budget_dropped += dropped;
            st.last_block_us = us as u32;
            st.last_prep_us = prep.as_micros() as u32;
            st.allowance = budget.allowance();
            s.allow_now.store(if lower { budget.allowance() } else { u32::MAX }, Ordering::Relaxed);
            st.voice_cap = cap;
            st.cuts = budget.cuts;
            st.keys_wanted = budget.keys_wanted;
            st.keys_kept = budget.keys_kept;
            st.fast_blocks = budget.fast;
            st.merged = budget.merged;
            st.merge_mean = if budget.merged > 0 { (budget.scale_sum / budget.merged as f64) as f32 } else { 1.0 };
            st.merge_max = budget.scale_max;
        }
        s.ring.lock().unwrap().blocks.push_back(out);
    }
}

/// Pushes of this many messages or more are ranked side by side (rankers()).
const SIDE_BY_SIDE: usize = 200_000;

/// One ranker per share of a big push: a quarter of the cores, 1 to 4 (the render thread, BASS and Spiderweb's
/// reader need theirs).
fn rankers() -> Vec<rank::Ranker> {
    let cores = std::thread::available_parallelism().map_or(4, |c| c.get());
    (0..(cores / 4).clamp(1, 4)).map(|_| rank::Ranker::new()).collect()
}

/// A played batch's memory back to the pool (a few kept: enough for the pushes in flight).
fn recycle(s: &Shared, b: Batch) {
    let mut pool = s.pool.lock().unwrap();
    if pool.len() < 4 {
        pool.push(b);
    }
}

/// Tests: frames a note sounds after its off, as the merging counts it (rank::TAIL).
pub fn set_tail(frames: u32) {
    rank::TAIL.store(frames, Ordering::Relaxed);
}

// ---- C ABI (Spiderweb's files/kestrel.py) ----

fn guard<T>(fallback: T, f: impl FnOnce() -> T) -> T {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)).unwrap_or_else(|_| {
        set_error("Kestrel's connector stopped unexpectedly");
        fallback
    })
}

/// "bridge version; Kestrel version + where it came from".
#[no_mangle]
pub extern "C" fn kb_version() -> *const c_char {
    static V: &CStr = c"0.9.0; Kestrel 1.4.0 (mbms0/kestrel-midi 844b002 moved onto 1.4.0 + Spiderweb's changes UnPrioritized/kestrel-midi 547e634)";
    V.as_ptr()
}

/// 1: Merlin pieces may carry repeat marks (Kestrel's `midi::REPEAT_MARK`, FF 7F 08 "SWn" key count before a note:
/// a stack of identical notes sent once). A connector without this function: notes written out (Spiderweb asks).
#[no_mangle]
pub extern "C" fn kb_repeat_marks() -> u32 {
    let _ = kestrel::midi::REPEAT_MARK;
    1
}

/// The last error text (kb_open failing, or the render thread's), UTF-8; valid until the next kb_* call.
#[no_mangle]
pub extern "C" fn kb_error(h: *const Synth) -> *const c_char {
    guard(std::ptr::null(), || {
        if !h.is_null() {
            if let Some(e) = unsafe { &*h }.error() {
                set_error(&e);
            }
        }
        LAST_ERROR.lock().unwrap_or_else(|e| e.into_inner()).as_ref().map_or(std::ptr::null(), |c| c.as_ptr())
    })
}

/// The caller's options; a shorter (older) struct: only the fields it has, defaults for the rest.
unsafe fn read_opts(opts: *const KbOptions) -> KbOptions {
    let mut o = KbOptions::default();
    if !opts.is_null() {
        let size = (*opts).size as usize;
        let len = size.min(std::mem::size_of::<KbOptions>());
        std::ptr::copy_nonoverlapping(opts as *const u8, &mut o as *mut KbOptions as *mut u8, len);
    }
    o
}

/// Null on failure, its text kept for kb_error.
fn or_null<T>(r: Result<T>) -> *mut T {
    match r {
        Ok(v) => Box::into_raw(Box::new(v)),
        Err(e) => {
            set_error(&format!("{e:#}"));
            std::ptr::null_mut()
        }
    }
}

/// Read one soundfont (UTF-8 path) into memory at `sample_rate`, no graphics card. Null on failure (kb_error).
/// Takes seconds: off the window's thread. Free with kb_font_free (synths and loaded sets keep what they need).
#[no_mangle]
pub unsafe extern "C" fn kb_font_load(path: *const c_char, sample_rate: u32) -> *mut FontBank {
    kb_font_load_with(path, sample_rate, 1)
}

/// `kb_font_load` with the soundfont's filters (1, Kestrel's default) or without (0: Settings → Sound's Filter).
#[no_mangle]
pub unsafe extern "C" fn kb_font_load_with(path: *const c_char, sample_rate: u32, filter: u32) -> *mut FontBank {
    collect_warnings();
    guard(std::ptr::null_mut(), || {
        if path.is_null() {
            set_error("no soundfont given");
            return std::ptr::null_mut();
        }
        match CStr::from_ptr(path).to_str() {
            Ok(s) => or_null(FontBank::load_with(std::path::Path::new(s), sample_rate, filter != 0)),
            Err(_) => {
                set_error("a soundfont path isn't UTF-8");
                std::ptr::null_mut()
            }
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn kb_font_free(f: *mut FontBank) {
    if !f.is_null() {
        guard((), || drop(Box::from_raw(f)))
    }
}

/// Stack `n` read soundfonts (top of Spiderweb's list first) and send them to the graphics card. Null on failure.
/// Under a second (the upload): still off the window's thread.
#[no_mangle]
pub unsafe extern "C" fn kb_load(fonts: *const *const FontBank, n: u32) -> *mut Loaded {
    kb_load_on(fonts, n, std::ptr::null(), std::ptr::null())
}

/// `kb_load` on one graphics card: `backend` + `card` (UTF-8, a line of `kb_cards`'s); null = Kestrel's own pick.
#[no_mangle]
pub unsafe extern "C" fn kb_load_on(
    fonts: *const *const FontBank,
    n: u32,
    backend: *const c_char,
    card: *const c_char,
) -> *mut Loaded {
    guard(std::ptr::null_mut(), || {
        let text = |p: *const c_char| {
            (!p.is_null()).then(|| CStr::from_ptr(p).to_string_lossy().into_owned()).filter(|s| !s.is_empty())
        };
        let list: Vec<&FontBank> = (0..n as usize).filter_map(|i| (*fonts.add(i)).as_ref()).collect();
        or_null(Loaded::new_on(&list, text(backend), text(card)))
    })
}

/// The graphics cards Kestrel can play on, best first (its own pick = the first): one line each,
/// "name\tbackend\tkind" (kind: discrete / integrated / virtual / other; software ones left out). Null on failure
/// (kb_error). Valid until the next kb_cards call. Takes up to a second: off the window's thread.
#[no_mangle]
pub extern "C" fn kb_cards() -> *const c_char {
    static CARDS: Mutex<Option<CString>> = Mutex::new(None);
    let cfg = Config { gpu_backend: own_pick().map(Into::into), ..Config::default() };  // (no GL instance: own_pick)
    guard(std::ptr::null(), || match kestrel::gpu::survey(&cfg) {
        Ok(survey) => {
            let lines: Vec<String> = survey
                .adapters
                .iter()
                .filter(|a| !a.is_software())
                .map(|a| {
                    // (by its name: wgpu isn't a dependency of ours)
                    let kind = match format!("{:?}", a.device_type).as_str() {
                        "DiscreteGpu" => "discrete",
                        "IntegratedGpu" => "integrated",
                        "VirtualGpu" => "virtual",
                        _ => "other",
                    };
                    let name = a.name.replace(['\t', '\n', '\r'], " ");
                    format!("{name}\t{}\t{kind}", format!("{:?}", a.backend).to_ascii_lowercase())
                })
                .collect();
            let mut keep = CARDS.lock().unwrap_or_else(|e| e.into_inner());
            *keep = CString::new(lines.join("\n")).ok();
            keep.as_ref().map_or(std::ptr::null(), |c| c.as_ptr())
        }
        Err(e) => {
            set_error(&format!("{e:#}"));
            std::ptr::null()
        }
    })
}

/// Free a loaded set; synths opened on it keep working (they hold their own share).
#[no_mangle]
pub unsafe extern "C" fn kb_unload(l: *mut Loaded) {
    if !l.is_null() {
        guard((), || drop(Box::from_raw(l)))
    }
}

/// The most Max voices this graphics card takes at `opts` (0 = not known, see kb_error).
#[no_mangle]
pub unsafe extern "C" fn kb_voices_top(l: *const Loaded, opts: *const KbOptions) -> u32 {
    if l.is_null() {
        return 0;
    }
    guard(0, || match (*l).voices_top(&read_opts(opts)) {
        Ok(v) => v,
        Err(e) => {
            set_error(&format!("{e:#}"));
            0
        }
    })
}

/// Rendered ahead: the longest any note of this set keeps sounding after its note-off, in seconds (every region's
/// release down to the envelope floor `opts` gives, as Merlin counts it; sound controllers like CC 72 not counted).
/// Below 0 = endless or not known (0.8.0 on).
#[no_mangle]
pub unsafe extern "C" fn kb_longest_release(l: *const Loaded, opts: *const KbOptions) -> f64 {
    if l.is_null() {
        return -1.0;
    }
    guard(-1.0, || {
        let l = &*l;
        let cfg = match config(&read_opts(opts)) {
            Ok(c) => c,
            Err(_) => return -1.0,
        };
        let mut most = 0usize;
        for p in &l.bank.params {
            let n = kestrel::merlin::kernel::release_frames(p, &cfg);
            if n == usize::MAX {
                return -1.0;
            }
            most = most.max(n);
        }
        most as f64 / l.sample_rate.max(1) as f64
    })
}

/// Open a synth on a loaded set (~20 ms). Null on failure (kb_error).
#[no_mangle]
pub unsafe extern "C" fn kb_open_on(l: *const Loaded, opts: *const KbOptions) -> *mut Synth {
    if l.is_null() {
        set_error("no soundfonts loaded");
        return std::ptr::null_mut();
    }
    guard(std::ptr::null_mut(), || or_null(Synth::open_on(&*l, &read_opts(opts))))
}

/// Open a synth: `fonts` = `n` UTF-8 paths, top of Spiderweb's list first. Null on failure (see kb_error).
/// Takes seconds (soundfonts load): call it off the window's thread. (= kb_font_load each + kb_load + kb_open_on)
#[no_mangle]
pub unsafe extern "C" fn kb_open(fonts: *const *const c_char, n: u32, opts: *const KbOptions) -> *mut Synth {
    guard(std::ptr::null_mut(), || {
        let o = read_opts(opts);
        let mut paths = Vec::new();
        for i in 0..n as usize {
            let p = *fonts.add(i);
            if p.is_null() {
                continue;
            }
            match CStr::from_ptr(p).to_str() {
                Ok(s) => paths.push(PathBuf::from(s)),
                Err(_) => {
                    set_error("a soundfont path isn't UTF-8");
                    return std::ptr::null_mut();
                }
            }
        }
        match Synth::open(&paths, &o) {
            Ok(s) => Box::into_raw(Box::new(s)),
            Err(e) => {
                set_error(&format!("{e:#}"));
                std::ptr::null_mut()
            }
        }
    })
}

/// Queue `n` messages: frames[i] = output frame, msgs[i] = status | d1 << 8 | d2 << 16 | port << 24,
/// tracks[i] = the track the budget shares by.
#[no_mangle]
pub unsafe extern "C" fn kb_push(h: *const Synth, frames: *const u64, msgs: *const u32, tracks: *const u16, n: u32) {
    if h.is_null() || n == 0 {
        return;
    }
    guard((), || {
        let n = n as usize;
        (*h).push(
            std::slice::from_raw_parts(frames, n),
            std::slice::from_raw_parts(msgs, n),
            std::slice::from_raw_parts(tracks, n),
        )
    })
}

/// `kb_push` with each note-on's length in frames (lens[i]; 0xFFFFFFFF = not known): the note budget keeps the
/// longest start of a key (a held note beside spam on one key).
#[no_mangle]
pub unsafe extern "C" fn kb_push_len(
    h: *const Synth,
    frames: *const u64,
    msgs: *const u32,
    tracks: *const u16,
    lens: *const u32,
    n: u32,
) {
    if h.is_null() || n == 0 {
        return;
    }
    guard((), || {
        let n = n as usize;
        let lens = (!lens.is_null()).then(|| std::slice::from_raw_parts(lens, n));
        (*h).push_len(
            std::slice::from_raw_parts(frames, n),
            std::slice::from_raw_parts(msgs, n),
            std::slice::from_raw_parts(tracks, n),
            lens,
        )
    })
}

/// `kb_push_len` (lens may be null) with each note-on standing for `copies[i]` exact copies (same frame, message,
/// port; null = 1 each): Spiderweb's feeder sends a stack once (Shinki's dense part: 4.5x fewer note-ons). The
/// copies sound together: their loudness adds as amplitudes. A note-on's copies have their note-offs left out by
/// the caller (one off ends it).
#[no_mangle]
pub unsafe extern "C" fn kb_push_copies(
    h: *const Synth,
    frames: *const u64,
    msgs: *const u32,
    tracks: *const u16,
    lens: *const u32,
    copies: *const u32,
    n: u32,
) {
    if h.is_null() || n == 0 {
        return;
    }
    guard((), || {
        let n = n as usize;
        let lens = (!lens.is_null()).then(|| std::slice::from_raw_parts(lens, n));
        let copies = (!copies.is_null()).then(|| std::slice::from_raw_parts(copies, n));
        (*h).push_full(
            std::slice::from_raw_parts(frames, n),
            std::slice::from_raw_parts(msgs, n),
            std::slice::from_raw_parts(tracks, n),
            lens,
            copies,
        )
    })
}

#[no_mangle]
pub unsafe extern "C" fn kb_cancel(h: *const Synth) {
    if !h.is_null() {
        guard((), || (*h).cancel())
    }
}

/// The loaded MIDI's messages from `frame` on taken back (`Synth::cut`): the frame they were taken back from, or
/// u64::MAX when the render thread didn't answer.
#[no_mangle]
pub unsafe extern "C" fn kb_cut(h: *const Synth, frame: u64) -> u64 {
    if h.is_null() {
        return u64::MAX;
    }
    guard(u64::MAX, || (*h).cut(frame).unwrap_or(u64::MAX))
}

/// The song being pushed (0 = none) and the output frame of its second 0 (`Synth::set_song`).
#[no_mangle]
pub unsafe extern "C" fn kb_song(h: *const Synth, origin: f64, song: u32) {
    if !h.is_null() {
        guard((), || (*h).set_song(origin, song))
    }
}

#[no_mangle]
pub unsafe extern "C" fn kb_set_lower(h: *const Synth, on: u32) {
    if !h.is_null() {
        guard((), || (*h).set_lower(on != 0))
    }
}

#[no_mangle]
pub unsafe extern "C" fn kb_stats(h: *const Synth, out: *mut KbStats) {
    if !h.is_null() && !out.is_null() {
        guard((), || *out = (*h).stats())
    }
}

/// Fill `frames` stereo frames of 32-bit float (for tests / writing without BASS).
#[no_mangle]
pub unsafe extern "C" fn kb_fill(h: *const Synth, out: *mut f32, frames: u32) {
    if !h.is_null() && !out.is_null() {
        guard((), || (*h).fill(std::slice::from_raw_parts_mut(out, frames as usize * 2)))
    }
}

/// Kestrel's own output stage (DC blocker + limiter, exactly as its renders run it) on a sound Spiderweb joins
/// itself: Rendered ahead's pieces are made without one, then the joined sound goes through this (user 2026-11-04:
/// Spiderweb's simpler limiter let a dense wall's fading end go dull and a noisy tail through; official renders
/// level them).
pub struct Stage {
    stage: kestrel::limiter::OutputStage,
    latency: u32,
    block: u64,
}

/// A new output stage from `opts`' limiter (0 off, 1 Brickwall, 2 Balanced), ceiling and DC blocker; null on bad
/// options. Free it with kb_stage_free.
#[no_mangle]
pub unsafe extern "C" fn kb_stage_new(opts: *const KbOptions) -> *mut Stage {
    if opts.is_null() {
        return std::ptr::null_mut();
    }
    guard(std::ptr::null_mut(), || {
        let Ok(mut cfg) = config(&*opts) else { return std::ptr::null_mut() };
        cfg.nan_guard = false;
        let latency = if cfg.limiter {
            kestrel::limiter::Brickwall::new(
                cfg.sample_rate,
                cfg.limiter_ceiling(),
                cfg.limiter_lookahead_ms,
                cfg.limiter_release_ms,
                cfg.limiter_sustain_ms,
                cfg.limiter_true_peak,
            )
            .with_hold_ms(cfg.sample_rate, cfg.limiter_hold_ms)
            .latency() as u32
        } else {
            0
        };
        Box::into_raw(Box::new(Stage { stage: kestrel::limiter::OutputStage::new(&cfg), latency, block: 0 }))
    })
}

/// Frames the stage holds the sound back by (the brickwall's look-ahead; 0 with the limiter off).
#[no_mangle]
pub unsafe extern "C" fn kb_stage_latency(h: *const Stage) -> u32 {
    if h.is_null() {
        0
    } else {
        (*h).latency
    }
}

/// `frames` interleaved stereo float frames through the stage, in place.
#[no_mangle]
pub unsafe extern "C" fn kb_stage_process(h: *mut Stage, buf: *mut f32, frames: u32) {
    if h.is_null() || buf.is_null() || frames == 0 {
        return;
    }
    guard((), || {
        let s = &mut *h;
        let _ = s.stage.process(std::slice::from_raw_parts_mut(buf, frames as usize * 2), s.block);
        s.block += 1;
    })
}

#[no_mangle]
pub unsafe extern "C" fn kb_stage_free(h: *mut Stage) {
    if !h.is_null() {
        drop(Box::from_raw(h));
    }
}

/// A BASS STREAMPROC: BASS_StreamCreate(rate, 2, BASS_SAMPLE_FLOAT, kb_streamproc, synth). Free the stream
/// BEFORE kb_close.
#[no_mangle]
pub unsafe extern "system" fn kb_streamproc(_handle: u32, buffer: *mut c_void, length: u32, user: *mut c_void) -> u32 {
    if user.is_null() || buffer.is_null() {
        return 0;
    }
    guard(0, || {
        let samples = length as usize / 4;
        (*(user as *const Synth)).fill(std::slice::from_raw_parts_mut(buffer as *mut f32, samples));
        length
    })
}

/// Stop the render thread and free everything.
#[no_mangle]
pub unsafe extern "C" fn kb_close(h: *mut Synth) {
    if !h.is_null() {
        guard((), || drop(Box::from_raw(h)))
    }
}

/// Rendered ahead: start making one piece (`midi` = UTF-8 path of a small MIDI file) into audio on a loaded set,
/// on its own thread. `engine` 0 = Kestrel, 1 = Merlin (refused for 31-EDO, Velocity threshold, 2+ soundfonts:
/// FAILED with kb_piece_error's text). `skip` frames dropped at the start, then at most `max_frames` kept (0 = to
/// the end, ring-out included). Returns at once; null on bad arguments (kb_error). Free with kb_piece_free.
#[no_mangle]
pub unsafe extern "C" fn kb_piece_start(
    l: *const Loaded,
    midi: *const c_char,
    opts: *const KbOptions,
    engine: u32,
    skip: u64,
    max_frames: u64,
) -> *mut piece::Piece {
    if l.is_null() || midi.is_null() {
        set_error("no soundfonts loaded or no MIDI file given");
        return std::ptr::null_mut();
    }
    guard(std::ptr::null_mut(), || {
        let path = PathBuf::from(CStr::from_ptr(midi).to_string_lossy().into_owned());
        let engine = if engine == 1 { piece::Engine::Merlin } else { piece::Engine::Kestrel };
        or_null(piece::Piece::start(&*l, &path, &read_opts(opts), engine, skip, max_frames))
    })
}

/// kb_piece_start with the piece's MIDI file given as bytes (`len` of them at `data`, copied before it returns: the
/// caller may free them at once), never written to disk. Same `engine`, `skip`, `max_frames`, result.
#[no_mangle]
pub unsafe extern "C" fn kb_piece_start_bytes(
    l: *const Loaded,
    data: *const u8,
    len: u64,
    opts: *const KbOptions,
    engine: u32,
    skip: u64,
    max_frames: u64,
) -> *mut piece::Piece {
    if l.is_null() || data.is_null() || len == 0 {
        set_error("no soundfonts loaded or no MIDI given");
        return std::ptr::null_mut();
    }
    guard(std::ptr::null_mut(), || {
        let bytes: std::sync::Arc<[u8]> = std::slice::from_raw_parts(data, len as usize).into();
        let engine = if engine == 1 { piece::Engine::Merlin } else { piece::Engine::Kestrel };
        or_null(piece::Piece::start_bytes(&*l, bytes, &read_opts(opts), engine, skip, max_frames))
    })
}

/// Where a piece is (state RUNNING 0 / DONE 1 / FAILED 2 / CANCELLED 3, frames kept, progress 0..1).
#[no_mangle]
pub unsafe extern "C" fn kb_piece_status(p: *const piece::Piece, out: *mut piece::KbPieceStatus) {
    if p.is_null() || out.is_null() {
        return;
    }
    guard((), || {
        let st = (*p).status();
        let size = ((*out).size as usize).min(std::mem::size_of::<piece::KbPieceStatus>());
        std::ptr::copy_nonoverlapping(&st as *const _ as *const u8, out as *mut u8, size);
        (*out).size = size as u32;
    })
}

/// Wait up to `ms` milliseconds for a piece to finish; its state.
#[no_mangle]
pub unsafe extern "C" fn kb_piece_wait(p: *const piece::Piece, ms: u32) -> u32 {
    if p.is_null() {
        return piece::FAILED;
    }
    guard(piece::FAILED, || (*p).wait(std::time::Duration::from_millis(ms as u64)))
}

/// Ask a piece to stop (its state turns CANCELLED soon; kb_piece_free waits for it).
#[no_mangle]
pub unsafe extern "C" fn kb_piece_cancel(p: *const piece::Piece) {
    if !p.is_null() {
        guard((), || (*p).cancel())
    }
}

/// A finished piece's audio: stereo interleaved f32, `*frames` frames; null before DONE. Valid until kb_piece_free.
#[no_mangle]
pub unsafe extern "C" fn kb_piece_audio(p: *const piece::Piece, frames: *mut u64) -> *const f32 {
    if p.is_null() {
        return std::ptr::null();
    }
    guard(std::ptr::null(), || {
        let (ptr, n) = (*p).audio();
        if !frames.is_null() {
            *frames = n as u64;
        }
        ptr
    })
}

/// Why a piece FAILED (UTF-8), null if it didn't; valid until the next kb_* call.
#[no_mangle]
pub unsafe extern "C" fn kb_piece_error(p: *const piece::Piece) -> *const c_char {
    if p.is_null() {
        return std::ptr::null();
    }
    guard(std::ptr::null(), || match (*p).error() {
        Some(e) => {
            set_error(&e);
            LAST_ERROR.lock().unwrap_or_else(|e| e.into_inner()).as_ref().map_or(std::ptr::null(), |c| c.as_ptr())
        }
        None => std::ptr::null(),
    })
}

/// Cancel (if running), wait for its thread, free it and its audio.
#[no_mangle]
pub unsafe extern "C" fn kb_piece_free(p: *mut piece::Piece) {
    if !p.is_null() {
        guard((), || drop(Box::from_raw(p)))
    }
}

/// (PFAviz++) A continuous Kestrel render on a loaded set (`stream.rs`): null on failure (kb_error). Free it with
/// kb_stream_free.
#[no_mangle]
pub unsafe extern "C" fn kb_stream_open(l: *const Loaded, opts: *const KbOptions) -> *mut stream::Stream {
    if l.is_null() {
        set_error("no soundfonts loaded");
        return std::ptr::null_mut();
    }
    guard(std::ptr::null_mut(), || or_null(stream::Stream::open(&*l, &read_opts(opts))))
}

/// `n` messages at their output frames (non-decreasing; frames from the stream's start), each note-on's
/// loudness multiplied by `scales[i]` (null: all 1).
#[no_mangle]
pub unsafe extern "C" fn kb_stream_push(h: *mut stream::Stream, frames: *const u64, msgs: *const u32, scales: *const f32, n: u32) {
    if h.is_null() || frames.is_null() || msgs.is_null() {
        return;
    }
    guard((), || {
        for i in 0..n as usize {
            let scale = if scales.is_null() { 1.0 } else { *scales.add(i) };
            (*h).push(*frames.add(i), *msgs.add(i), scale);
        }
    })
}

/// A SysEx (`len` bytes from its F0) at `frame`.
#[no_mangle]
pub unsafe extern "C" fn kb_stream_sysex(h: *mut stream::Stream, frame: u64, data: *const u8, len: u32) {
    if !h.is_null() && !data.is_null() {
        guard((), || (*h).push_sysex(frame, std::slice::from_raw_parts(data, len as usize)))
    }
}

/// The next `frames` stereo frames into `out`: 1, or 0 on failure (kb_error).
#[no_mangle]
pub unsafe extern "C" fn kb_stream_render(h: *mut stream::Stream, out: *mut f32, frames: u32) -> u32 {
    if h.is_null() || out.is_null() {
        return 0;
    }
    guard(0, || match (*h).render(std::slice::from_raw_parts_mut(out, frames as usize * 2)) {
        Ok(()) => 1,
        Err(e) => {
            set_error(&format!("{e:#}"));
            0
        }
    })
}

/// The voices allowed from the next block on (1 to the options' max_voices).
#[no_mangle]
pub unsafe extern "C" fn kb_stream_set_cap(h: *mut stream::Stream, cap: u32) {
    if !h.is_null() {
        guard((), || (*h).set_cap(cap))
    }
}

/// The voices sounding after the last block.
#[no_mangle]
pub unsafe extern "C" fn kb_stream_voices(h: *const stream::Stream) -> u64 {
    if h.is_null() {
        return 0;
    }
    guard(0, || (*h).voices())
}

#[no_mangle]
pub unsafe extern "C" fn kb_stream_free(h: *mut stream::Stream) {
    if !h.is_null() {
        guard((), || drop(Box::from_raw(h)))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn part_type_message_makes_a_drum_part_event() {
        // (Spiderweb's own shapes keep channel 10 melodic: port 2, channel 10, melodic)
        let msg = PART_TYPE as u32 | 9 << 8 | 0 << 16 | 2 << 24;
        assert!(matches!(decode(msg), Some(Event::DrumPart { ch: 0x29, map: 0 })));
        assert!(matches!(decode(0x90 | 60 << 8 | 100 << 16), Some(Event::NoteOn { ch: 0, key: 60, vel: 100 })));
    }
}
