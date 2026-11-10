//! Each block's note-ons put in PRIORITY ORDER when they are pushed (on the caller's thread), so the render
//! thread only cuts the order at its allowance: tiers + fair-by-track choice on the render thread took ~5 ms of a
//! 10.7 ms block in Shinki's dense part (185,000 note-ons a block), which held the allowance at its floor.
//!
//! The order (budget.rs has the user's rules): tier 1 = one start of each key (the longest, then the loudest),
//! tier 2 = one of each key on each channel, tier 3 = one of each key at each frame (the loudest there), tier 4
//! = the rest. Inside tiers 2 to 4 the tracks take TURNS, each track loudest first (earlier first at one
//! velocity): any first N of a round-robin is the fair split (a track with fewer notes runs out and leaves its
//! turns to the others). Each note-on also gets the ranks of its key's two kept candidates, where its loudness
//! goes if it is dropped (`srep` the same key at the same frame, `krep` the key's tier-1 note, always kept).
//!
//! TIER 3 KEEPS THE RHYTHM (2026-10-27): Shinki's spam starts stacks ~94 times a second; with every note playing,
//! the quiet stacks between the loud ones cancel part of the loud ones' low buzz. Loudest-first dropped the quiet
//! stacks, and their loudness carried to a note at ANOTHER frame can't cancel anything: 60-120 Hz came out
//! +2.7 dB (F#2 +6) against every note played. One note per key per frame, carrying its frame's dropped copies:
//! -0.1 dB (dev/data/kestrel-plan.md).
//!
//! A block is ranked from the events of ONE push with frames in it, in a row. A block whose note-ons came in
//! two pushes, or late into a later block, doesn't match its `Span` and is chosen on the render thread instead.

use crate::Pending;

/// Note-ons in a block past which it is ranked whatever the allowance is now (a wall arriving while there is no
/// limit yet must be ranked when the first cut comes).
pub const RANK_ALWAYS: usize = 20_000;
/// Frames of a note counted when a dropped copy's loudness is shared by length (~2 s at 48 kHz: a piano note
/// has faded by then; a note whose end isn't known counts this long).
pub const HEARD: u32 = 96_000;

/// Frames a note keeps sounding after its note-off (its release): a 2 ms spam note still makes ~0.5 s of sound.
/// Counted by note length alone, spam copies merged into a long note added almost nothing (Shinki: merging on
/// or off measured the same, 2026-10-27).
pub static TAIL: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(24_000);

/// A copy's sounding time (length + TAIL) against the note it is merged into (both capped at HEARD + TAIL): 1 =
/// as long.
pub fn len_ratio(copy: u32, into: u32) -> f32 {
    ratio_with(TAIL.load(std::sync::atomic::Ordering::Relaxed), copy, into)
}

fn ratio_with(tail: u32, copy: u32, into: u32) -> f32 {
    let (c, t) = (copy.min(HEARD) + tail, into.min(HEARD) + tail);
    (c.max(1) as f32 / t.max(1) as f32).min(1.0)
}

/// MERGED BY TIME (user 2026-10-27, after the low keys came out 4-6 dB too loud): all of a stack's loudness on
/// its longest note for that note's whole length gave long-ringing low notes far too much. A copy adds as an
/// amplitude only on the same frame, at the same velocity AND about as long (`COHERENT`), else as power times
/// its share of the time.
pub const COHERENT: f32 = 0.9;

/// A note-on as the ranking reads it: packed small (a Shinki block's ~185,000 fit the CPU's cache; reading them
/// out of the batch's messages all over memory was most of the ~4.6 ms a block ranking took, 2026-10-27).
#[derive(Clone, Copy)]
struct On {
    frame: u64,
    /// its place in the group's messages
    i: u32,
    len: u32,
    slot: u16,
    /// its frame inside the block
    at: u16,
    track: u16,
    key: u8,
    vel: u8,
}

const NONE: u32 = u32::MAX;

/// A ranked group in its batch: messages `start..start + len`, `ons` note-ons ranked 0..ons. The render thread
/// finding its block is exactly one chooses it in one pass over the messages, no checking pass (a Shinki block
/// is ~20 MB of messages: reading them twice was most of the render thread's ~2.9 ms of choosing, 2026-10-27).
#[derive(Clone, Copy, Debug)]
pub struct Span {
    pub start: u32,
    pub len: u32,
    pub ons: u32,
}

pub struct Ranker {
    // (below: indices into `ons`)
    by_key: [u32; 128],
    by_slot: Vec<u32>,
    /// (frame inside the block x 128 + key)
    by_frame: Vec<u32>,
    ons: Vec<On>,
    t1: Vec<u32>,
    t2: Vec<u32>,
    t3: Vec<u32>,
    t4: Vec<u32>,
    order: Vec<u32>,
    rank_of: Vec<u32>,
    // round-robin scratch
    per_track: Vec<u32>,
    starts: Vec<u32>,
    tracks: Vec<u16>,
    sorted: Vec<u32>,
    by_vel: Vec<u32>,
}

impl Ranker {
    pub fn new() -> Self {
        Ranker {
            by_key: [NONE; 128],
            by_slot: vec![NONE; 256 * 128],
            by_frame: Vec::new(),
            ons: Vec::new(),
            t1: Vec::new(),
            t2: Vec::new(),
            t3: Vec::new(),
            t4: Vec::new(),
            order: Vec::new(),
            rank_of: Vec::new(),
            per_track: vec![0; 1 << 16],
            starts: vec![0; 1 << 16],
            tracks: Vec::new(),
            sorted: Vec::new(),
            by_vel: Vec::new(),
        }
    }

    /// Rank the note-ons of every block in `batch` with more than `over` of them (frames in a row of one block =
    /// one group; the others stay unranked: chosen on the render thread if they ever need choosing). Each ranked
    /// group's place is added to `spans`, in order.
    pub fn rank(&mut self, batch: &mut [Pending], block_frames: u64, over: usize, spans: &mut Vec<Span>) {
        let mut a = 0;
        if self.by_frame.len() < block_frames as usize * 128 {
            self.by_frame = vec![NONE; block_frames as usize * 128];
        }
        while a < batch.len() {
            // (block bounds compared, not each frame divided: a division per message was ~1/3 of the ranking)
            let start = batch[a].frame / block_frames * block_frames;
            let end = start + block_frames;
            self.ons.clear();
            let mut b = a;
            while b < batch.len() && (start..end).contains(&batch[b].frame) {
                let p = &batch[b];
                if p.msg & 0xF0 == 0x90 && (p.msg >> 16) & 0x7F != 0 {
                    let key = ((p.msg >> 8) & 0x7F) as u8;
                    self.ons.push(On {
                        frame: p.frame,
                        i: (b - a) as u32,
                        len: p.len,
                        slot: ((((p.msg >> 24) & 0x0F) << 4 | (p.msg & 0x0F)) as u16) * 128 + key as u16,
                        at: (p.frame - start) as u16,
                        track: p.track,
                        key,
                        vel: ((p.msg >> 16) & 0x7F) as u8,
                    });
                }
                b += 1;
            }
            if self.ons.len() > over {
                self.rank_group(&mut batch[a..b]);
                spans.push(Span { start: a as u32, len: (b - a) as u32, ons: self.ons.len() as u32 });
            }
            a = b;
        }
    }

    fn rank_group(&mut self, g: &mut [Pending]) {
        let ons = std::mem::take(&mut self.ons);
        let rank = |o: &On| (o.len, o.vel);
        for (j, o) in ons.iter().enumerate() {
            let k = &mut self.by_key[o.key as usize];
            if *k == NONE || rank(o) > rank(&ons[*k as usize]) {
                *k = j as u32;
            }
            let s = &mut self.by_slot[o.slot as usize];
            if *s == NONE || rank(o) > rank(&ons[*s as usize]) {
                *s = j as u32;
            }
            let f = &mut self.by_frame[o.at as usize * 128 + o.key as usize];
            if *f == NONE || rank(o) > rank(&ons[*f as usize]) {
                *f = j as u32;
            }
        }
        self.t1.clear();
        self.t2.clear();
        self.t3.clear();
        self.t4.clear();
        for (j, o) in ons.iter().enumerate() {
            let j = j as u32;
            if self.by_key[o.key as usize] == j {
                self.t1.push(j);
            } else if self.by_slot[o.slot as usize] == j {
                self.t2.push(j);
            } else if self.by_frame[o.at as usize * 128 + o.key as usize] == j {
                self.t3.push(j);
            } else {
                self.t4.push(j);
            }
        }
        self.order.clear();
        let t1 =std::mem::take(&mut self.t1);
        self.order.extend_from_slice(&t1);
        self.t1 = t1;
        let t2 = std::mem::take(&mut self.t2);
        self.round_robin(&ons, &t2);
        self.t2 = t2;
        let t3 = std::mem::take(&mut self.t3);
        self.round_robin(&ons, &t3);
        self.t3 = t3;
        let t4 = std::mem::take(&mut self.t4);
        self.round_robin(&ons, &t4);
        self.t4 = t4;
        self.rank_of.clear();
        self.rank_of.resize(ons.len(), 0);
        for (r, &j) in self.order.iter().enumerate() {
            self.rank_of[j as usize] = r as u32;
        }
        let tail = TAIL.load(std::sync::atomic::Ordering::Relaxed);
        for (j, o) in ons.iter().enumerate() {
            // (a dropped note's loudness goes to its own frame's note of the key: it sounds when it did)
            let s = self.by_frame[o.at as usize * 128 + o.key as usize] as usize;
            let k = self.by_key[o.key as usize] as usize;
            let (so, ko) = (&ons[s], &ons[k]);
            let p = &mut g[o.i as usize];
            p.rank = self.rank_of[j];
            p.srep = self.rank_of[s];
            p.krep = self.rank_of[k];
            // (the same sound = same frame AND same velocity: other velocities may be other samples, which add
            // as powers; adding all same-frame copies as amplitudes left Shinki's F#2 / G2 6 dB too loud against
            // every note played with 4 M voices, 2026-10-27)
            p.s_same = so.frame == o.frame && so.vel == o.vel;
            p.k_same = ko.frame == o.frame && ko.vel == o.vel;
            p.s_ratio = ratio_with(tail, o.len, so.len);
            p.k_ratio = ratio_with(tail, o.len, ko.len);
        }
        self.by_key = [NONE; 128];
        for o in &ons {
            self.by_slot[o.slot as usize] = NONE;
            self.by_frame[o.at as usize * 128 + o.key as usize] = NONE;
        }
        self.ons = ons;
    }

    /// Append `list` to `order`: tracks taking turns, each track's notes loudest first (stable). Counting sorts
    /// only (a dense block has ~200,000).
    fn round_robin(&mut self, ons: &[On], list: &[u32]) {
        if list.is_empty() {
            return;
        }
        // loudest first, stable
        let mut hist = [0u32; 129];
        for &j in list {
            hist[127 - ons[j as usize].vel as usize + 1] += 1;
        }
        for v in 1..129 {
            hist[v] += hist[v - 1];
        }
        self.by_vel.clear();
        self.by_vel.resize(list.len(), 0);
        for &j in list {
            let v = 127 - ons[j as usize].vel as usize;
            self.by_vel[hist[v] as usize] = j;
            hist[v] += 1;
        }
        // then grouped by track, stable (so loudest first inside each)
        self.tracks.clear();
        for &j in &self.by_vel {
            let t = ons[j as usize].track as usize;
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
        self.sorted.clear();
        self.sorted.resize(list.len(), 0);
        for &j in &self.by_vel {
            let t = ons[j as usize].track as usize;
            self.sorted[self.starts[t] as usize] = j;
            self.starts[t] += 1;
        }
        // turns: tracks with the most notes first (then the higher track number: the render thread's equal
        // shares leave the remainder to those too), so each round is a prefix of the track list
        let mut tracks: Vec<(u32, u16, u32)> = self
            .tracks
            .iter()
            .map(|&t| (self.per_track[t as usize], t, self.starts[t as usize] - self.per_track[t as usize]))
            .collect();
        tracks.sort_unstable_by(|x, y| (y.0, y.1).cmp(&(x.0, x.1)));
        let mut active = tracks.len();
        let mut round = 0u32;
        while active > 0 {
            while active > 0 && tracks[active - 1].0 <= round {
                active -= 1;
            }
            for &(_, _, start) in &tracks[..active] {
                self.order.push(self.sorted[(start + round) as usize]);
            }
            round += 1;
        }
        for &t in &self.tracks {
            self.per_track[t as usize] = 0;
        }
    }
}
