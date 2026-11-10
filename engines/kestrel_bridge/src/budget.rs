//! "Lower it by itself": how many note-ons a block may take, learned from whether blocks keep up with real time,
//! and which ones go when there are more. FAIR BY TRACK (user 2026-10-27): every track with notes in the block
//! gets an equal share (a track needing less leaves the rest to the others); inside a track the quietest go
//! first. A dropped note-on's note-off is dropped too (paired in order per channel + key).
//!
//! Block time alone misleads (measured 2026-10-27, RTX 3080 Ti): a lightly used graphics card slows itself down,
//! so easy blocks paced in real time take ~4 ms of 10.7 against ~1.2 ms back to back; a busy one runs at full
//! speed (Shinki's dense part: 29 ms either way). So "behind" = block time near real time OR the blocks kept
//! ready running out. Shinki before its wall: ~185,000 note-ons a block; even at 16,384 voices the note-ons
//! alone fill a block's time, so dropping new notes is what keeps real time.

use crate::Pending;

/// Each channel's volume (CC 7) and expression (CC 11) as the messages pass, so a merged copy counts with its
/// OWN channel's level (user 2026-10-27: Shinki has layers at volume 0; merged into an audible channel's note
/// they came out at full volume). GM defaults 100 / 127; Kestrel's curve: gain = (cc / 127)^2 each.
#[derive(Clone, Copy)]
pub struct Level {
    vol: [u8; 256],
    expr: [u8; 256],
}

impl Level {
    fn new() -> Self {
        Level { vol: [100; 256], expr: [127; 256] }
    }

    fn ch(msg: u32) -> usize {
        ((((msg >> 24) & 0x0F) << 4) | (msg & 0x0F)) as usize
    }

    /// A control change passing.
    pub fn cc(&mut self, msg: u32) {
        let v = ((msg >> 16) & 0x7F) as u8;
        match (msg >> 8) & 0x7F {
            7 => self.vol[Self::ch(msg)] = v,
            11 => self.expr[Self::ch(msg)] = v,
            121 => {
                // (Reset All Controllers: expression back; volume stays, as in GM)
                self.expr[Self::ch(msg)] = 127;
            }
            _ => {}
        }
    }

    /// Amplitude factor of the message's channel.
    pub fn gain(&self, msg: u32) -> f64 {
        let c = Self::ch(msg);
        let (v, e) = (self.vol[c] as f64 / 127.0, self.expr[c] as f64 / 127.0);
        v * v * e * e
    }
}

/// A kept note-on's loudness factor: its own `copies` (amplitude `a` each, together as one amplitude) plus what was
/// merged into it (`coh` amplitude, `pow` power), against its own amplitude played once. A note on a silent
/// channel carries nothing (as sent); no_merge (tests) = only its own copies.
fn merged_scale(a: f64, copies: u32, coh: f64, pow: f64, no_merge: bool) -> f32 {
    let own = a * copies as f64;
    if a <= 1e-9 {
        1.0
    } else if no_merge || (coh <= 0.0 && pow <= 0.0) {
        copies as f32
    } else {
        (((own + coh).powi(2) + pow).sqrt() / a) as f32
    }
}

/// A queue of yes / no kept as runs (a stack's copies are dropped in a row: thousands of entries in one run).
#[derive(Clone, Default)]
struct Runs(std::collections::VecDeque<(bool, u32)>);

impl Runs {
    fn push_back(&mut self, v: bool) {
        match self.0.back_mut() {
            Some((b, n)) if *b == v => *n += 1,
            _ => self.0.push_back((v, 1)),
        }
    }

    fn pop_front(&mut self) -> Option<bool> {
        let (v, n) = self.0.front_mut()?;
        let v = *v;
        *n -= 1;
        if *n == 0 {
            self.0.pop_front();
        }
        Some(v)
    }
}

/// Block time as a share of its real-time length (running average): over HIGH = fewer notes, under TARGET =
/// more again.
const HIGH: f64 = 0.9;
const TARGET: f64 = 0.7;
/// Fewest note-ons a block may take while lowered (128 = one start of every key always fits: tier 1), and
/// fewest voices.
const LEAST: f64 = 128.0;
/// (voices cost little under ~65k: Shinki's dense part 10.7 ms a block at 16k, 11.9 at 65k, 29 at 1M; a cap of
/// ~4,000 stole every ringing note: the held lines in the user's reference render were gone, 2026-10-27; 86k still
/// left Shinki's loudest keys 6-15 dB quiet against the user's render, 200k much closer)
const LEAST_VOICES: f64 = 200_000.0;
/// Blocks a kept note should get to ring (~0.5 s): new note-ons a block at most voices / RING. Measured timing-free
/// against the user's render (Shinki's dense part, loudest keys): 20,000 a block with 1 M voices 1.0 dB off, 4,000
/// with 200k 1.8 dB, but 8,000 with 200k 7.5 dB (more starts than voices: each stolen before it was heard).
const RING: f64 = 50.0;
/// Most a block raises the limits by, with all its time to spare.
const CLIMB: f64 = 0.08;
/// Weight of the newest block in the running averages.
const SMOOTH: f64 = 0.25;
/// Blocks with sound not judged at the start: the first one sounding does the graphics card's one-time setup
/// and took ~10x a normal block (Shinki: judged, its one cut took 185,000 note-ons a block down to 39).
const WARM: u64 = 1;
/// (`Budget::peak`: ~1.3 s to halve at 512 frames a block)
const PEAK_FADE: f64 = 0.994;
/// Blocks the learned limits rule after a real shortage while following remembered ones (~0.25 s).
const TROUBLE_BLOCKS: u32 = 24;

pub struct Budget {
    /// Note-ons a block may take; None = no limit.
    allow: Option<f64>,
    /// Note-ons the last block asked for.
    wanted: usize,
    /// The most note-ons a block asked for lately (fading by PEAK_FADE a block): the limit goes only once far above
    /// it (by the last block alone, a few light blocks in Shinki lifted it and the next dense one took 200 ms).
    peak: f64,
    /// Running averages: block time / real time, and how empty the ready blocks were (0 full .. 1 none).
    load: f64,
    empty: f64,
    blocks: u64,
    /// Note-ons a block may take that last proved too many (climbed past slowly), and blocks left before
    /// another cut may come.
    ceiling: Option<f64>,
    cooldown: u32,
    /// The limit was raised since the last cut; the averages start over after the cut's wait.
    raised: bool,
    fresh: bool,
    /// Cuts so far.
    pub cuts: u32,
    ahead: usize,
    /// Voices Kestrel may sound now (None = the user's Max voices), and that Max.
    cap: Option<f64>,
    max_voices: u32,
    /// Per channel (0..256) x key: each note-on still waiting for its off, in order, and whether it was dropped.
    /// PAIRED IN ORDER (as a MIDI file is read): counters ("a kept note takes the next off") gave a stack's
    /// LONG kept note the first short copy's off, cutting it short on the most stacked keys (D#7 in Shinki
    /// -9 dB against every note played, user heard "note cutting everywhere", 2026-10-27).
    held: Vec<Runs>,
    ons: Vec<usize>,
    keep: Vec<bool>,
    /// Different keys that wanted to start / still started, summed over blocks (runs lost show here).
    pub keys_wanted: u64,
    pub keys_kept: u64,
    // (scratch for tiers / choose: counting sorts, no per-block allocation)
    by_slot: Vec<usize>,
    coh: Vec<f64>,
    pow: Vec<f64>,
    /// Loudness factor of a kept note carrying merged copies (1 = as sent).
    scale: Vec<f32>,
    /// `filter_ranked`: the messages that stay (index in the block; a kept note-on's own amplitude, its copies).
    stay: Vec<(u32, f32, u32)>,
    t1: Vec<usize>,
    t2: Vec<usize>,
    t3: Vec<usize>,
    per_track: Vec<u32>,
    tracks: Vec<u16>,
    by_track: Vec<usize>,
    starts: Vec<u32>,
    /// Blocks chosen from the push-time order (`ranked`), not by `tiers`; kept note-ons sent carrying merged
    /// copies (factor over 1).
    pub fast: u64,
    pub merged: u64,
    pub scale_sum: f64,
    pub scale_max: f32,
    /// Tests: a fixed allowance (`fix`), 0 = none; merging off.
    fixed: u32,
    no_merge: bool,
    /// Channel levels as the messages passed (the general way reads them as of the block's start).
    level: Level,
    /// This block follows remembered limits (`follow`); a cut while following = trouble: the learned limits
    /// (lower) rule until they climb back to the remembered ones, at most TROUBLE_BLOCKS blocks (counted down).
    following: bool,
    trouble: u32,
}

impl Budget {
    pub fn new(max_voices: u32) -> Self {
        Budget {
            allow: None,
            wanted: 0,
            peak: 0.0,
            load: 0.0,
            empty: 0.0,
            blocks: 0,
            ceiling: None,
            cooldown: 0,
            raised: true,
            fresh: false,
            cuts: 0,
            ahead: 2,
            cap: None,
            max_voices,
            held: vec![Runs::default(); 256 * 128],
            ons: Vec::new(),
            keep: Vec::new(),
            keys_wanted: 0,
            keys_kept: 0,
            by_slot: vec![usize::MAX; 256 * 128],
            coh: Vec::new(),
            pow: Vec::new(),
            scale: Vec::new(),
            stay: Vec::new(),
            t1: Vec::new(),
            t2: Vec::new(),
            t3: Vec::new(),
            per_track: vec![0; 1 << 16],
            tracks: Vec::new(),
            by_track: Vec::new(),
            starts: vec![0; 1 << 16],
            fast: 0,
            merged: 0,
            scale_sum: 0.0,
            scale_max: 0.0,
            fixed: 0,
            no_merge: false,
            level: Level::new(),
            following: false,
            trouble: 0,
        }
    }

    /// The limits for the next block: (note-ons, voices), INFINITY = none.
    pub fn limits(&self) -> (f32, f32) {
        (self.allow.map_or(f32::INFINITY, |a| a as f32), self.cap.map_or(f32::INFINITY, |c| c as f32))
    }

    /// Note-ons the last block asked for.
    pub fn wanted(&self) -> usize {
        self.wanted
    }

    /// A block chosen in two calls (render_loop `apart`): the first call's note-ons counted with the second's.
    pub fn add_wanted(&mut self, n: usize) {
        self.wanted += n;
    }

    /// The next block takes the limits it had on an earlier play (`limits` then): the same notes kept, the same
    /// sound every play. After a cut while following (this PC slower now) the learned limits rule until they are
    /// back up to the remembered ones.
    pub fn follow(&mut self, allow: f32, cap: f32) {
        if self.trouble > 0 {
            // (back to them once the learned ones climbed there, or after TROUBLE_BLOCKS: climbing from a deep cut
            // took the rest of a 6 s play in Shinki, every later block different from the first play)
            self.trouble -= 1;
            if self.allow.map_or(f32::INFINITY, |a| a as f32) < allow && self.trouble > 0 {
                return;
            }
            self.trouble = 0;
        }
        self.allow = allow.is_finite().then_some(allow as f64);
        self.cap = cap.is_finite().then_some(cap as f64);
        self.following = true;
    }

    /// A start (Play / a jump): no trouble carried over.
    pub fn restart(&mut self) {
        self.trouble = 0;
        self.following = false;
    }

    /// Every note's pairing forgotten: a cancel (the offs pushed ahead are gone; Spiderweb ends what sounds itself).
    /// Kept, an old "dropped" ate a later kept note's off and it rang on, every key then an off behind (hunt
    /// 2026-10-27: Stop + Play in Shinki's busy part, notes piled up).
    pub fn forget(&mut self) {
        for r in &mut self.held {
            r.0.clear();
        }
    }

    /// A channel's pairing forgotten at its all notes off / all sound off (CC 123 / 120): nothing sounds there now
    /// (a wall's silence: Spiderweb leaves its notes' offs out).
    fn forget_channel(&mut self, msg: u32) {
        if msg & 0xF0 != 0xB0 || !matches!((msg >> 8) & 0x7F, 120 | 123) {
            return;
        }
        let ch = ((((msg >> 24) & 0x0F) << 4) | (msg & 0x0F)) as usize;
        for r in &mut self.held[ch * 128..ch * 128 + 128] {
            r.0.clear();
        }
    }

    pub fn allowance(&self) -> u32 {
        self.allow.map_or(u32::MAX, |a| a as u32)
    }

    /// Tests: a fixed allowance (0 = learned as usual), the voice cap left at Max voices; `merge` false = dropped
    /// notes' loudness not carried (to measure what the merging itself does).
    pub fn fix(&mut self, allow: u32, merge: bool) {
        self.fixed = allow;
        self.no_merge = !merge;
    }

    /// Voices Kestrel may sound now (the user's Max voices when not lowered).
    pub fn voice_cap(&self) -> u32 {
        self.cap.map_or(self.max_voices, |c| c as u32)
    }

    /// Hand `push` the block's messages that stay, each with its loudness factor (amplitude: copies merged into
    /// a note-on); returns how many note-ons were dropped. `ranked` = Some(its note-ons) when the block is
    /// exactly one group ranked at push (rank::Span).
    pub fn filter(&mut self, block: &[Pending], ranked: Option<usize>, lower: bool, mut push: impl FnMut(&Pending, f32)) -> u64 {
        // one ranked group over the allowance: the lean way (no arrays the size of the block: a Shinki block of
        // ~370,000 messages took 1.75 ms the general way, mostly zeroing them)
        if let (Some(allow), Some(n)) = (self.allow.filter(|_| lower), ranked) {
            if n > allow as usize {
                return self.filter_ranked(block, n, allow as usize, &mut push);
            }
        }
        self.ons.clear();
        for (i, p) in block.iter().enumerate() {
            if p.msg & 0xF0 == 0x90 && (p.msg >> 16) & 0x7F != 0 {
                self.ons.push(i);
            }
        }
        self.wanted = self.ons.len();
        self.keep.clear();
        self.keep.resize(block.len(), true);
        let mut dropped = 0u64;
        if let Some(allow) = self.allow.filter(|_| lower) {
            let allow = allow as usize;
            if self.ons.len() > allow {
                dropped = (self.ons.len() - allow) as u64;
                self.tiers(block, allow);
            }
        }
        // (how many different keys wanted to start, and how many still do: runs lost show here)
        let mut want_keys = [false; 128];
        let mut kept_keys = [false; 128];
        for &i in &self.ons {
            let key = ((block[i].msg >> 8) & 0x7F) as usize;
            want_keys[key] = true;
            kept_keys[key] |= self.keep[i];
        }
        self.keys_wanted += want_keys.iter().filter(|&&k| k).count() as u64;
        self.keys_kept += kept_keys.iter().filter(|&&k| k).count() as u64;
        for (i, p) in block.iter().enumerate() {
            let status = p.msg & 0xF0;
            let is_on = status == 0x90 && (p.msg >> 16) & 0x7F != 0;
            let is_off = status == 0x80 || (status == 0x90 && !is_on);
            if is_on || is_off {
                let ch = ((((p.msg >> 24) & 0x0F) << 4) | (p.msg & 0x0F)) as usize;
                let k = ch * 128 + ((p.msg >> 8) & 0x7F) as usize;
                if is_on {
                    self.held[k].push_back(!self.keep[i]);
                } else if self.held[k].pop_front() == Some(true) {
                    continue; // (its note-on was dropped)
                }
            } else if status == 0xB0 {
                self.level.cc(p.msg);
                self.forget_channel(p.msg);
            }
            if self.keep[i] {
                // (a kept note-on carries the loudness of the copies merged into it)
                push(p, if is_on { self.scale.get(i).copied().unwrap_or(p.copies as f32) } else { 1.0 });
            }
        }
        self.scale.clear();
        dropped
    }

    /// `filter` for a block that is one group ranked at push, `n` note-ons over `allow`: kept = rank < allow, in
    /// ONE pass over the messages (a Shinki block is ~15 MB of them): what stays is listed on the way, each kept
    /// note-on with its channel's level then, and handed out after, once the copies' loudness is all summed.
    fn filter_ranked(&mut self, block: &[Pending], n: usize, allow: usize, push: &mut impl FnMut(&Pending, f32)) -> u64 {
        let amp = |p: &Pending| {
            let v = ((p.msg >> 16) & 0x7F) as f64 / 127.0;
            v * v
        };
        self.coh.clear();
        self.coh.resize(allow, 0.0);
        self.pow.clear();
        self.pow.resize(allow, 0.0);
        self.stay.clear();
        self.fast += 1;
        self.wanted = n;
        let mut want_keys = [false; 128];
        let mut kept_keys = [false; 128];
        for (i, p) in block.iter().enumerate() {
            let status = p.msg & 0xF0;
            let on = status == 0x90 && (p.msg >> 16) & 0x7F != 0;
            let off = status == 0x80 || (status == 0x90 && !on);
            if !on && !off {
                if status == 0xB0 {
                    self.level.cc(p.msg);
                    self.forget_channel(p.msg);
                }
                self.stay.push((i as u32, 0.0, 1));
                continue;
            }
            let ch = ((((p.msg >> 24) & 0x0F) << 4) | (p.msg & 0x0F)) as usize;
            let key = ((p.msg >> 8) & 0x7F) as usize;
            let k = ch * 128 + key;
            if on {
                want_keys[key] = true;
                if (p.rank as usize) < allow {
                    kept_keys[key] = true;
                    self.held[k].push_back(false);
                    self.stay.push((i as u32, (amp(p) * self.level.gain(p.msg)) as f32, p.copies));
                } else {
                    self.held[k].push_back(true);
                    // its loudness (with its own channel's volume) to its key's kept note on its frame, else the
                    // key's tier-1 note, as an amplitude when it is the same sound (known from push)
                    let (to, same, ratio) = if (p.srep as usize) < allow {
                        (p.srep, p.s_same, p.s_ratio)
                    } else {
                        (p.krep, p.k_same, p.k_ratio)
                    };
                    // (tier 1 always fits a real allowance: under 128 only in tests, the loudness is lost then)
                    if let Some(to) = Some(to as usize).filter(|&t| t < allow) {
                        // (its copies together: one amplitude)
                        let a = amp(p) * self.level.gain(p.msg) * p.copies as f64;
                        if same && ratio >= crate::rank::COHERENT {
                            self.coh[to] += a;
                        } else {
                            self.pow[to] += a * a * ratio as f64;
                        }
                    }
                }
            } else if self.held[k].pop_front() != Some(true) {
                // (its own note-on was kept, or one from before Kestrel heard of it)
                self.stay.push((i as u32, 0.0, 1));
            }
        }
        for &(i, a, copies) in &self.stay {
            let p = &block[i as usize];
            let is_on = p.msg & 0xF0 == 0x90 && (p.msg >> 16) & 0x7F != 0;
            if !is_on {
                push(p, 1.0);
                continue;
            }
            let r = p.rank as usize;
            let (c, w, a) = (self.coh[r], self.pow[r], a as f64);
            let scale = merged_scale(a, copies, c, w, self.no_merge);
            if scale > 1.0 {
                self.merged += 1;
                self.scale_sum += scale as f64;
                self.scale_max = self.scale_max.max(scale);
            }
            push(p, scale);
        }
        self.keys_wanted += want_keys.iter().filter(|&&k| k).count() as u64;
        self.keys_kept += kept_keys.iter().filter(|&&k| k).count() as u64;
        (n - allow) as u64
    }

    /// Keep `allow` of the block's note-ons, in tiers (user 2026-10-27: thinned by loudness alone, the runs
    /// running up and down were gone: walls are mostly STACKED COPIES of one key): 1. the loudest start of each
    /// key (any channel; at most 128, so every pitch keeps sounding), 2. the loudest of each key on each
    /// channel (other instruments on that key), 3. one of each key at each frame (the rhythm), 4. the rest. A
    /// tier that doesn't fit whole is shared fairly
    /// by track (`choose`); the tiers after it are dropped.
    fn tiers(&mut self, block: &[Pending], allow: usize) {
        let key = |i: usize| ((block[i].msg >> 8) & 0x7F) as usize;
        let slot = |i: usize| ((((block[i].msg >> 24) & 0x0F) << 4 | (block[i].msg & 0x0F)) as usize) * 128 + key(i);
        // the start kept for a key: the LONGEST, then the loudest (user 2026-10-27: a held note beside spam on one
        // key lost to the louder spam: its line was gone from the spectrogram)
        let rank = |i: usize| (block[i].len, (block[i].msg >> 16) & 0x7F);
        let mut by_key = [usize::MAX; 128];
        for &i in &self.ons {
            let b = &mut by_key[key(i)];
            if *b == usize::MAX || rank(i) > rank(*b) {
                *b = i;
            }
        }
        for &i in &self.ons {
            let b = &mut self.by_slot[slot(i)];
            if *b == usize::MAX || rank(i) > rank(*b) {
                *b = i;
            }
        }
        // (3. one of each key at each frame: rank.rs "TIER 3 KEEPS THE RHYTHM")
        let by_frame = Self::frame_reps(block, &self.ons);
        let (mut t1, mut t2, mut t3) = (std::mem::take(&mut self.t1), std::mem::take(&mut self.t2), std::mem::take(&mut self.t3));
        let mut t4 = Vec::new();
        t1.clear();
        t2.clear();
        t3.clear();
        for &i in &self.ons {
            if by_key[key(i)] == i {
                t1.push(i);
            } else if self.by_slot[slot(i)] == i {
                t2.push(i);
            } else if by_frame[&(block[i].frame, key(i))] == i {
                t3.push(i);
            } else {
                t4.push(i);
            }
        }
        for &i in &self.ons {
            self.by_slot[slot(i)] = usize::MAX;
        }
        let mut left = allow;
        for tier in [&t1, &t2, &t3, &t4] {
            if tier.len() <= left {
                left -= tier.len();
            } else {
                self.choose(block, tier, left);
                left = 0;
            }
        }
        (self.t1, self.t2, self.t3) = (t1, t2, t3);
        self.merge_loudness(block);
    }

    /// Each (frame, key)'s note that rank.rs puts in tier 3: the longest, then the loudest.
    fn frame_reps(block: &[Pending], ons: &[usize]) -> std::collections::HashMap<(u64, usize), usize> {
        let rank = |i: usize| (block[i].len, (block[i].msg >> 16) & 0x7F);
        let mut reps = std::collections::HashMap::new();
        for &i in ons {
            let b = reps.entry((block[i].frame, ((block[i].msg >> 8) & 0x7F) as usize)).or_insert(i);
            if rank(i) > rank(*b) {
                *b = i;
            }
        }
        reps
    }

    /// MERGED, NOT JUST DROPPED (user 2026-10-27): each dropped copy's loudness goes to a kept note of its key
    /// (its frame's if that one is kept, else the key's tier-1 note), which is played louder: a key stacked many
    /// times stays louder than its neighbours, as in a full render. Kestrel's velocity curve at full tracking:
    /// amplitude = (vel / 127)^2 (bank.rs velocity_atten_cb). Copies on the same frame add as amplitudes (the
    /// same sound starting together), on other frames as powers.
    fn merge_loudness(&mut self, block: &[Pending]) {
        // (channel levels as of the block's start: this way is for blocks split over pushes, rare)
        let level = self.level;
        let amp = |i: usize| {
            let v = ((block[i].msg >> 16) & 0x7F) as f64 / 127.0;
            v * v * level.gain(block[i].msg)
        };
        let key = |i: usize| ((block[i].msg >> 8) & 0x7F) as usize;
        // (its frame's note of the key if kept, else the longest kept note of the key takes the loudness)
        let rank = |i: usize| (block[i].len, (block[i].msg >> 16) & 0x7F);
        let by_frame = Self::frame_reps(block, &self.ons);
        let mut by_key = [usize::MAX; 128];
        for &i in &self.ons {
            if self.keep[i] {
                let k = &mut by_key[key(i)];
                if *k == usize::MAX || rank(i) > rank(*k) {
                    *k = i;
                }
            }
        }
        // per kept note: amplitude on its own frame, power from other frames
        self.coh.clear();
        self.coh.resize(block.len(), 0.0);
        self.pow.clear();
        self.pow.resize(block.len(), 0.0);
        let mut any = false;
        for &i in &self.ons {
            if self.keep[i] {
                continue;
            }
            let s = by_frame[&(block[i].frame, key(i))];
            let t = if self.keep[s] { s } else { by_key[key(i)] };
            if t == usize::MAX {
                continue;
            }
            any = true;
            // (shared by time, as rank.rs COHERENT says)
            let ratio = crate::rank::len_ratio(block[i].len, block[t].len);
            let same_vel = (block[i].msg >> 16) & 0x7F == (block[t].msg >> 16) & 0x7F;
            let a = amp(i) * block[i].copies as f64; // (its copies together: one amplitude)
            if block[i].frame == block[t].frame && same_vel && ratio >= crate::rank::COHERENT {
                self.coh[t] += a;
            } else {
                self.pow[t] += a * a * ratio as f64;
            }
        }
        self.scale.clear();
        self.scale.resize(block.len(), 1.0);
        for &i in &self.ons {
            if self.keep[i] {
                // (FULL merging, user 2026-10-27: our Kestrel change push_scaled multiplies the voice's gain, no
                // velocity-127 ceiling and the note's own velocity layer; the quick version's raised velocity left
                // the most stacked keys 14-19 dB short)
                let (c, w) = if any { (self.coh[i], self.pow[i]) } else { (0.0, 0.0) };
                self.scale[i] = merged_scale(amp(i), block[i].copies, c, w, false);
            }
        }
    }

    /// Mark all but `allow` of `cands` as dropped: equal shares per track, loudest kept inside each (same
    /// velocity: the earlier one). Counting sorts only: a dense block has ~200,000 note-ons.
    fn choose(&mut self, block: &[Pending], cands: &[usize], allow: usize) {
        if allow == 0 {
            for &i in cands {
                self.keep[i] = false;
            }
            return;
        }
        // note-ons grouped by track (stable: block order inside each)
        self.tracks.clear();
        for &i in cands {
            let t = block[i].track as usize;
            if self.per_track[t] == 0 {
                self.tracks.push(t as u16);
            }
            self.per_track[t] += 1;
        }
        let mut at = 0u32;
        for &t in &self.tracks {
            self.starts[t as usize] = at;
            at += self.per_track[t as usize];
        }
        self.by_track.clear();
        self.by_track.resize(cands.len(), 0);
        for &i in cands {
            let t = block[i].track as usize;
            self.by_track[self.starts[t] as usize] = i;
            self.starts[t] += 1;
        }
        // shares: smallest tracks first, each takes min(its count, an equal share of what's left)
        let mut order: Vec<(u32, u16)> = self.tracks.iter().map(|&t| (self.per_track[t as usize], t)).collect();
        order.sort_unstable();
        let mut left = allow;
        let total = order.len();
        let mut take = vec![0u32; 0];
        take.resize(total, 0);
        for (n, &(count, _)) in order.iter().enumerate() {
            let share = left / (total - n);
            let k = (count as usize).min(share);
            take[n] = k as u32;
            left -= k;
        }
        // inside each track: keep its `take` loudest (velocity histogram, earlier first at the cut)
        for (n, &(count, t)) in order.iter().enumerate() {
            let k = take[n] as usize;
            let end = self.starts[t as usize] as usize; // (moved to the group's end by the fill above)
            let group = &self.by_track[end - count as usize..end];
            if k >= group.len() {
                continue;
            }
            let mut hist = [0u32; 128];
            for &i in group {
                hist[((block[i].msg >> 16) & 0x7F) as usize] += 1;
            }
            // lowest velocity `cut` kept, and how many at it
            let (mut cut, mut room) = (127usize, k);
            while cut > 0 && hist[cut] as usize <= room {
                room -= hist[cut] as usize;
                cut -= 1;
            }
            for &i in group {
                let v = ((block[i].msg >> 16) & 0x7F) as usize;
                if v < cut || (v == cut && {
                    let fits = room > 0;
                    room = room.saturating_sub(1);
                    !fits
                }) {
                    self.keep[i] = false;
                }
            }
        }
        for &t in &self.tracks {
            self.per_track[t as usize] = 0;
        }
    }

    /// A block took `load` of its real-time length to make, with `ready` of `ahead` blocks waiting when it
    /// started.
    pub fn measured(&mut self, load: f64, ready: usize, ahead: usize, voices: u64, lower: bool) {
        self.ahead = ahead;
        let cuts = self.cuts;
        self.judge(load, ready, ahead, voices, lower);
        if self.following && self.cuts != cuts {
            self.trouble = TROUBLE_BLOCKS;
        }
        self.following = false;
    }

    fn judge(&mut self, load: f64, ready: usize, ahead: usize, voices: u64, lower: bool) {
        if self.fixed != 0 {
            self.allow = Some(self.fixed as f64);
            self.cap = None;
            return;
        }
        if self.blocks == 0 && self.wanted == 0 && voices == 0 {
            return; // (silence before the first note: nothing to learn)
        }
        self.blocks += 1;
        self.peak = (self.peak * PEAK_FADE).max(self.wanted as f64);
        // (one block is always being taken by the output: ahead - 1 ready = all well)
        let empty = 1.0 - (ready as f64 / (ahead.max(2) - 1) as f64).min(1.0);
        if self.blocks == 1 || (self.fresh && self.cooldown == 0) {
            (self.load, self.empty) = (load, empty);
            self.fresh = false;
        } else {
            self.load += (load - self.load) * SMOOTH;
            self.empty += (empty - self.empty) * SMOOTH;
        }
        if !lower {
            self.allow = None;
            self.cap = None;
            return;
        }
        if self.blocks <= WARM {
            return;
        }
        if self.cooldown > 0 {
            self.cooldown -= 1;
            return;
        }
        // (one slow block with every ready block still waiting = harmless: in the real window one block a second
        // took ~11 ms of 10.7 and each cut the allowance by 30 %, 7,600 down to 900 in 10 s = the melody 8 dB
        // quiet against the user's render, 2026-10-27)
        let slow = load > 1.5 || (load > 1.0 && ready + 1 < ahead.max(2));
        // (following limits that kept up on an earlier play: only a real shortage cuts, the ready blocks nearly
        // gone; any slow block cut them, and no two plays were the same, 2026-10-28)
        let trouble = if self.following {
            load > 1.0 && ready <= 1
        } else {
            slow || self.load > HIGH || self.empty > 0.5
        };
        if self.following && !trouble {
            return;
        }
        if trouble {
            // (in proportion to how far behind this block was: a block 2.7x too slow cuts to ~30 % at once; a
            // fixed 0.7 took ~30 slow blocks to come down from Shinki's 185,000 a block = 0.7 s of silence)
            let base = self.allow.map_or(self.wanted as f64, |a| a.min(self.wanted as f64));
            let factor = (HIGH * 0.9 / load.max(1e-6)).min(0.7);
            // the level that was too much: climbed past only slowly from now on (it swung every ~75 ms
            // otherwise, each swing a 5-9 ms gap: user heard it, 2026-10-27). Only a level that was CLIMBED
            // to counts: cuts in a row for one problem pushed it down to nothing.
            if self.raised {
                self.ceiling = Some(base * 0.9);
            }
            self.raised = false;
            self.allow = Some((base * factor).max(LEAST));
            // and the voices sounding: notes already ringing kept blocks slow after every new note was dropped
            // (Shinki: ~1 M voices from the first blocks); Kestrel steals the quietest down to the cap. Only once
            // the ready blocks are being used up: one slow block with them all waiting (Shinki's wall ending,
            // 516,000 note-offs in one block, 19 ms) cut 300,000 ringing notes at once, heard (user 2026-10-28)
            if ready + 1 < ahead.max(2) || self.empty > 0.5 {
                let sounding = self.cap.map_or(voices as f64, |c| c.min(voices as f64));
                self.cap = Some((sounding * factor).max(LEAST_VOICES));
            }
            // notes already started keep blocks slow for a while (a piano note rings on): let the cut show
            // before judging again (= the blocks kept ready), and judge only blocks made after it (the average
            // still held the slow blocks: three cuts in a row took Shinki from 185,000 note-ons a block to 36)
            self.cooldown = self.ahead as u32;
            self.fresh = true;
            self.cuts += 1;
        } else if self.load < TARGET && self.empty < 0.2 {
            if let Some(a) = self.allow {
                // in proportion to the time to spare: up to 8 % a block far below TARGET (~2x a quarter
                // second; 3 % climbed ~2x a second), slower near it; past the last trouble level a tenth of that
                let spare = (TARGET - self.load) / TARGET;
                let step = CLIMB * spare * if self.ceiling.is_some_and(|c| a >= c) { 0.1 } else { 1.0 };
                let next = a * (1.0 + step) + 1.0;
                self.raised = true;
                // no limit once it's far above what blocks ask for
                self.allow = if next > self.peak.max(1.0) * 4.0 { None } else { Some(next) };
            }
            if let Some(c) = self.cap {
                // the voice cap back up the same way, up to the user's Max voices
                let spare = (TARGET - self.load) / TARGET;
                let next = c * (1.0 + CLIMB * spare) + 16.0;
                self.cap = if next >= self.max_voices as f64 { None } else { Some(next) };
            }
            // the old trouble level is forgotten slowly (a lighter part may follow)
            if let Some(c) = self.ceiling.as_mut() {
                *c *= 1.0005;
            }
        }
        // no more starts than the voices can let ring (only once lowered: unlimited stays unlimited)
        if let Some(a) = self.allow {
            self.allow = Some(a.min(self.voice_cap() as f64 / RING).max(LEAST));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn on(track: u16, key: u32, vel: u32) -> Pending {
        Pending::new(0, 0x90 | key << 8 | vel << 16, track)
    }

    fn off(key: u32) -> Pending {
        Pending::new(0, 0x80 | key << 8, 0)
    }

    /// The same blocks chosen from the push-time order give what the render-thread choice gives (both ways,
    /// every allowance), merged loudness included.
    #[test]
    fn ranked_at_push_chooses_the_same() {
        let mut seed = 7u64;
        let mut rnd = |n: u32| {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            (seed % n as u64) as u32
        };
        for case in 0..40 {
            let n = 50 + rnd(3000) as usize;
            let mut block: Vec<Pending> = (0..n)
                .map(|_| {
                    let mut p = on(rnd(9) as u16, 30 + rnd(24), 1 + rnd(127));
                    p.msg |= rnd(3) << 24 | rnd(2);
                    p.frame = rnd(4) as u64;
                    p.len = if rnd(3) == 0 { crate::LONG } else { rnd(5000) };
                    p
                })
                .collect();
            block.sort_by_key(|p| p.frame);
            let mut ranked = block.clone();
            crate::rank::Ranker::new().rank(&mut ranked, 512, 0, &mut Vec::new());
            for allow in [128usize, 200, 700, n.saturating_sub(1)] {
                if allow >= n {
                    continue; // (nothing to drop: neither way runs)
                }
                let (mut a, mut b) = (Budget::new(1 << 20), Budget::new(1 << 20));
                a.allow = Some(allow as f64);
                b.allow = Some(allow as f64);
                let (mut x, mut y) = (Vec::new(), Vec::new());
                a.filter(&mut block.clone(), None, true, |p, s| x.push((p.msg, p.track, p.frame, (s * 1e4).round() as i64)));
                b.filter(&mut ranked.clone(), Some(n), true, |p, s| y.push((p.msg, p.track, p.frame, (s * 1e4).round() as i64)));
                assert!(b.fast > 0, "case {case}: the ranked block wasn't taken the fast way");
                x.sort();
                y.sort();
                assert_eq!(x.len(), y.len(), "case {case} allow {allow}");
                // (which copy of a velocity is kept may differ: counts per channel + key + velocity + track agree,
                // and every key's merged loudness)
                let tally = |v: &[(u32, u16, u64, i64)]| {
                    let mut t: Vec<(u32, u16)> = v.iter().map(|e| (e.0, e.1)).collect();
                    t.sort();
                    t
                };
                assert_eq!(tally(&x), tally(&y), "case {case} allow {allow}");
                let loud = |v: &[(u32, u16, u64, i64)]| {
                    let mut per = std::collections::BTreeMap::new();
                    for e in v {
                        let a = ((e.0 >> 16) & 0x7F) as f64 / 127.0;
                        *per.entry((e.0 >> 8) & 0x7F).or_insert(0.0) += a * a * e.3 as f64 / 1e4;
                    }
                    per.into_iter().map(|(k, v)| (k, (v * 1e3).round() as i64)).collect::<Vec<_>>()
                };
                assert_eq!(loud(&x), loud(&y), "case {case} allow {allow}: merged loudness per key");
            }
        }
    }

    /// Offs are paired in order: a dropped copy's off is dropped even while a kept note on its key sounds (the
    /// kept note then ends at its own off, not the copy's).
    #[test]
    fn a_dropped_copys_off_never_ends_the_kept_note() {
        let mut b = Budget::new(1 << 20);
        b.allow = Some(127.0);
        // key 60: a short copy first (dropped: the held note is longer), then the held note
        let mut block: Vec<Pending> = (0..127).map(|k| on(1, k, 50)).collect();
        block.push(Pending { len: 10, ..on(1, 60, 50) });
        block[60].len = crate::LONG;
        // (block[60] = the held note, block[127] = the short copy, sent after it)
        let mut kept = Vec::new();
        b.filter(&block, None, true, |p, _| kept.push(p.len));
        assert!(!kept.contains(&10), "the short copy was kept");
        // the next block: the FIRST off on key 60 belongs to the held note (it started first), the second to the
        // dropped copy: in order, the held note ends at the first, the second is dropped
        let offs = vec![off(60), off(60)];
        let mut sent = 0;
        b.allow = None;
        b.filter(&offs, None, true, |_, _| sent += 1);
        assert_eq!(sent, 1);
    }

    #[test]
    fn a_held_note_beats_louder_spam_on_its_key() {
        let mut b = Budget::new(1 << 20);
        b.allow = Some(128.0);
        // key 60: 500 loud spam notes 100 frames long, and one quiet held note
        let mut block: Vec<Pending> = (0..500).map(|_| Pending { len: 100, ..on(1, 60, 120) }).collect();
        block.push(on(2, 60, 40));
        let mut kept = Vec::new();
        b.filter(&mut block, None, true, |p, s| kept.push((p.track, (p.msg >> 16) & 0x7F, s)));
        // the held note is kept (tier 1) and carries the spam's loudness FOR THE TIME IT SOUNDED (length + release
        // tail, rank::len_ratio): 127 of the spam are kept themselves (allowance 128), 373 merged:
        // sqrt(a^2 + 373 x b^2 x ratio) / a with a = (40/127)^2, b = (120/127)^2 (not all of it at once: ~3,900)
        let held = kept.iter().find(|k| k.0 == 2).expect("the held note was dropped");
        assert_eq!(held.1, 40);
        let (a, b) = ((40.0f64 / 127.0).powi(2), (120.0f64 / 127.0).powi(2));
        let ratio = crate::rank::len_ratio(100, crate::LONG) as f64;
        assert!(ratio > 0.1 && ratio < 0.5, "{ratio}");
        let want = ((a * a + 373.0 * b * b * ratio).sqrt() / a) as f32;
        assert!((held.2 - want).abs() < 0.01, "{held:?}, want {want}");
    }

    #[test]
    fn fair_shares_and_loudest_kept() {
        let mut b = Budget::new(1 << 20);
        b.allow = Some(4.0);
        // track 1: a wall of 10 quiet notes; track 2: two loud melody notes
        let mut block: Vec<Pending> = (0..10).map(|k| on(1, 40 + k, 10 + k)).collect();
        block.push(on(2, 80, 100));
        block.push(on(2, 81, 90));
        let mut kept = Vec::new();
        let dropped = b.filter(&mut block, None, true, |p, _| kept.push(p.msg));
        assert_eq!(dropped, 8);
        // the melody's two notes + the wall's two loudest
        let keys: Vec<u32> = kept.iter().map(|m| (m >> 8) & 0x7F).collect();
        assert_eq!(keys, vec![48, 49, 80, 81]);
    }

    #[test]
    fn a_quiet_run_survives_a_loud_stack() {
        let mut b = Budget::new(1 << 20);
        b.allow = Some(128.0);
        // track 1: 1,000 loud copies of key 60; track 2: a quiet run note on key 72; track 3: a copy of 72
        let mut block: Vec<Pending> = (0..1000).map(|_| on(1, 60, 127)).collect();
        block.push(on(2, 72, 30));
        block.push(on(3, 72, 20));
        let mut kept = Vec::new();
        b.filter(&mut block, None, true, |p, _| kept.push(((p.msg >> 8) & 0x7F, (p.msg >> 16) & 0x7F)));
        assert_eq!(kept.len(), 128);
        assert!(kept.contains(&(72, 30)), "the run note was dropped");
        assert!(!kept.contains(&(72, 20)) || kept.iter().filter(|k| k.0 == 72).count() == 2);
        assert_eq!((b.keys_wanted, b.keys_kept), (2, 2));
    }

    #[test]
    fn dropped_copies_make_the_kept_one_louder() {
        let mut b = Budget::new(1 << 20);
        b.allow = Some(128.0);
        // 129 different keys can't happen (128 keys), so: 128 keys once + a second copy of key 0, same frame
        let mut block: Vec<Pending> = (0..128).map(|k| on(1, k, 50)).collect();
        block.push(on(1, 0, 50));
        let mut kept = Vec::new();
        assert_eq!(b.filter(&mut block, None, true, |p, s| kept.push(((p.msg >> 8) & 0x7F, (p.msg >> 16) & 0x7F, s))), 1);
        // two copies on one frame = the kept one at its own velocity, twice the amplitude
        assert!(kept.contains(&(0, 50, 2.0)), "{:?}", &kept[..3]);
        assert!(kept.contains(&(1, 50, 1.0)));
        // on different frames they add as powers: sqrt(2)
        let mut block: Vec<Pending> = (0..128).map(|k| on(1, k, 50)).collect();
        block.push(Pending { frame: 7, ..on(1, 0, 50) });
        let mut kept = Vec::new();
        b.filter(&mut block, None, true, |p, s| kept.push(((p.msg >> 8) & 0x7F, s)));
        let s = kept.iter().find(|k| k.0 == 0).unwrap().1;
        assert!((s - std::f32::consts::SQRT_2).abs() < 1e-5, "{s}");
    }

    #[test]
    fn same_velocity_keeps_the_earlier() {
        let mut b = Budget::new(1 << 20);
        b.allow = Some(3.0);
        let mut block: Vec<Pending> = (0..6).map(|k| on(7, 40 + k, 64)).collect();
        let mut kept = Vec::new();
        b.filter(&mut block, None, true, |p, _| kept.push((p.msg >> 8) & 0x7F));
        assert_eq!(kept, vec![40, 41, 42]);
        // (scratch left clean for the next block)
        let mut block: Vec<Pending> = (0..2).map(|k| on(7, 60 + k, 64)).collect();
        let mut kept = Vec::new();
        b.filter(&mut block, None, true, |p, _| kept.push((p.msg >> 8) & 0x7F));
        assert_eq!(kept, vec![60, 61]);
    }

    #[test]
    fn a_dropped_note_loses_its_off() {
        let mut b = Budget::new(1 << 20);
        b.allow = Some(1.0);
        let mut block = vec![on(1, 60, 100), on(1, 60, 5)];
        let mut kept = Vec::new();
        b.filter(&mut block, None, true, |p, _| kept.push(p.msg));
        assert_eq!(kept.len(), 1);
        // first off pairs with the kept (first) note-on, the second with the dropped one
        let mut block = vec![off(60), off(60)];
        let mut kept = Vec::new();
        b.allow = None;
        b.filter(&mut block, None, true, |p, _| kept.push(p.msg));
        assert_eq!(kept.len(), 1);
    }

    /// Hunt 2026-10-27: a dropped note still waiting for its off when the sound was stopped (cancel / all notes
    /// off) ate the off of the next KEPT note on its key: it rang on.
    #[test]
    fn a_stop_forgets_dropped_notes() {
        for how in ["cancel", "cc123", "cc120"] {
            let mut b = Budget::new(1 << 20);
            b.allow = Some(1.0);
            // (the quiet one first: dropped, its "dropped" left at the front of the key's queue)
            let mut block = vec![on(1, 60, 5), on(1, 60, 100)];
            b.filter(&mut block, None, true, |_, _| {});
            b.allow = None;
            match how {
                "cancel" => b.forget(),
                _ => {
                    let cc = Pending::new(0, 0xB0 | if how == "cc123" { 123 } else { 120 } << 8, 0);
                    b.filter(&[cc], None, true, |_, _| {});
                }
            }
            let mut block = vec![on(1, 60, 100), off(60)];
            let mut kept = Vec::new();
            b.filter(&mut block, None, true, |p, _| kept.push(p.msg));
            assert_eq!(kept.len(), 2, "{how}: the new note's off was eaten");
        }
    }

    #[test]
    fn off_means_everything_plays() {
        let mut b = Budget::new(1 << 20);
        b.allow = Some(1.0);
        let mut block: Vec<Pending> = (0..5).map(|k| on(1, 40 + k, 50)).collect();
        let mut n = 0;
        assert_eq!(b.filter(&mut block, None, false, |_, _| n += 1), 0);
        assert_eq!(n, 5);
    }

    /// Measures (ignored): one Shinki-sized ranked block through `filter`, by part.
    #[test]
    #[ignore = "measures"]
    fn shinki_block_time() {
        let mut block = Vec::new();
        for i in 0..185_000u32 {
            let mut p = on((i % 3000) as u16, 20 + i % 88, 1 + (i * 7) % 127);
            p.msg |= (i % 4) << 24 | (i % 16);
            p.frame = (i % 512) as u64;
            p.len = 50 + i % 400;
            block.push(p);
        }
        block.sort_by_key(|p| p.frame);
        let offs: Vec<Pending> = block.iter().map(|p| Pending { msg: p.msg & !0xF0 & !(0x7F << 16) | 0x80, ..*p }).collect();
        crate::rank::Ranker::new().rank(&mut block, 4096, 0, &mut Vec::new());
        let mut all = block.clone();
        all.extend(offs);
        let mut b = Budget::new(1 << 20);
        b.allow = Some(4500.0);
        for round in 0..3 {
            let t = std::time::Instant::now();
            let mut n = 0;
            b.filter(&all, Some(185_000), true, |_, _| n += 1);
            eprintln!("round {round}: {} messages, {n} kept, filter {:.2?} (fast {})", all.len(), t.elapsed(), b.fast);
        }
        let t = std::time::Instant::now();
        let _ = all.clone();
        eprintln!("copying the block: {:.2?}", t.elapsed());
    }

    #[test]
    fn many_tracks_quickly() {
        // a Shinki-sized block: 200,000 note-ons over 5,000 tracks
        let mut b = Budget::new(1 << 20);
        b.allow = Some(20_000.0);
        let mut block: Vec<Pending> =
            (0..200_000u32).map(|i| on((i % 5000) as u16, i % 128, 1 + (i * 7) % 127)).collect();
        let t = std::time::Instant::now();
        let mut n = 0;
        let dropped = b.filter(&mut block, None, true, |_, _| n += 1);
        assert_eq!(dropped, 180_000);
        assert_eq!(n, 20_000);
        eprintln!("200,000 note-ons chosen in {:.2?}", t.elapsed());
        assert!(t.elapsed().as_millis() < 20);
    }
}
