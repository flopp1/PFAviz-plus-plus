// The fast note renderer (DESIGN.md 8): the same visible set, key lighting and pixels as the
// original's MainScreen (OracleEngine), from indexed queries on the visual store instead of from
// replaying events.
//
// Each key is composited independently over pixel rows: walking notes from the highest index down
// (the painter's order reversed), only notes that own at least one still-unpainted row are kept.
// Those "owners" are then drawn with the original DrawNote, in the original order (white keys, then
// black keys, ascending index). A pixel's final colour depends only on the last note drawn over it,
// and that note is always an owner, so the result is bit-identical to drawing every note, while
// the work and the vertex count are bounded by rows x keys rather than by the number of notes.
#pragma once

#include <atomic>
#include <climits>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/ThreadPool.h"
#include "render/Renderer.h"
#include "song/Song.h"
#include "visual/Effects.h"
#include "visual/LodCache.h"
#include "visual/Scene.h"

class FastEngine
{
public:
    struct FrameStats
    {
        size_t iOwners = 0, iQueries = 0, iKeys = 0;
        bool bOrdered = false;          // some neighbouring keys could share a pixel column
        bool bView = false;             // drawn from the hidden-channel draw view
        double dSweepMs = 0.0;          // all of DrawNotes
        double dQueryMs = 0.0;          // the parallel per-key part
        size_t iLodBuckets = 0;         // buckets swept from the LOD cache
        double dSweepCpuMs = 0.0, dVertCpuMs = 0.0;  // CPU time summed over threads: finding owners, making their vertices
        double dLodBuildMs = 0.0;       // LOD segments built for this frame (synchronous mode), not in the times above
        double dPixels = 0.0;           // screen pixels the note rects cover (with overdraw)
        size_t iRects = 0, iMerged = 0; // rects (instances) drawn; rects merged into the one before
    };

    explicit FastEngine( const Song &song, int iThreads = 0 );
    ~FastEngine();

    // Hidden channels by slot (track * 16 + channel). With any hidden, a draw view without them is
    // built in the background (or at once with bSync), and frames switch to it when it is ready
    // (DESIGN.md 8.10). Until then:
    //   - Full store (songs with weird notes, nothing culled): drawn with the hidden notes skipped,
    //     so the new mask shows at once;
    //   - Compact store (culled with every channel visible, so skipping is not exact): drawn as
    //     before the change, from the previous view or the full store.
    // DrawMask() is the mask the last frame was drawn with (empty: nothing hidden).
    void SetHidden( const std::vector< uint8_t > &vHiddenSlot, bool bSync = false );
    bool ViewPending() const;
    const std::vector< uint8_t > &DrawMask() const { return m_vDrawMask; }

    // The LOD cache (DESIGN.md 8.11): built in the background for the current display setting, used
    // where ready. bSync (tools): what a frame needs is built before it is drawn.
    void SetLod( bool bOn, bool bSync = false );
    // Drawing threads from the next frame (0: one per logical processor)
    void SetThreads( int iThreads ) { m_iThreads = Parallel::ThreadCount( iThreads ); }
    // FrameStats::dPixels is computed only when asked for (it costs about a third of building the vertices)
    void SetPixelStats( bool bOn ) { m_bPixelStats = bOn; }
    // Consecutive one-colour rects of a key that meet edge to edge are drawn as one (exact; on by default)
    void SetMergeRows( bool bOn ) { m_bMergeRows = bOn; }
    size_t LodBytes() const { return m_pLod ? m_pLod->MemoryBytes() : 0; }
    bool LodOn() const { return m_bLod; }
    bool DrawsPreviousMask() const { return m_vDrawMask != m_vMask; }   // the last frame waited for the view

    // The NoteEngine sequence of MainScreen::Logic (see visual/NoteEngine.h)
    void Reset();
    void AdvanceEnd( long long llEndTime );
    void Process( long long llStartTime );
    void Jump( long long llTarget, long long llClampedStart, long long llEndTime );
    void DrawNotes( Renderer *pRenderer, const FrameGlobals &g, const std::vector< TrackSettings > &vTracks );
    // Track * 16 + channel of the note lighting each key, or -1 (hidden channels included, R8); with
    // pStart, also when that note started
    void GetKeySlots( int aSlot[256], long long *pStart = nullptr ) const;
    // Effects (visual/Effects.h): DrawNotes draws the notes playing at the hit line lit, and collects
    // their glow rects (Glow, until the next DrawNotes). Null: off.
    void SetLight( const Effects::NoteLight *pLight ) { m_pLight = pLight; }
    const std::vector< Renderer::GlowRect > &Glow() const { return m_vGlow; }

    const FrameStats &LastStats() const { return m_Stats; }

private:
    // A note that owns at least one pixel row, with what drawing it needs. Its rects are drawn only
    // over rows [iRowA, iRowB), the span of the rows it owns (iRowA == INT_MIN: drawn whole).
    // bShort: its inner rect is empty (at most 2 * deflate rows tall), so what it draws is just its
    // outer rect over that span, in the very dark colour
    struct Owner { uint64_t llPos; long long llStart, llEnd; uint32_t uSlot; int iRowA, iRowB; bool bShort; };
    struct KeyCols { int iKey; int iLo, iHi; bool bDegenerate; };          // a key's pixel columns this frame
    struct MergeItem { uint64_t llU, llOrder; int iKey; uint64_t llIdx; };   // an owner of keys that may share a column
    struct Bounds
    {
        size_t iJ, iB, iE;              // first note at/after the jump; end of the sounding part; end of the upcoming part
        long long llEth;                // a sounding note must end after this
        uint64_t llUEth1;               // first unique tick with time > llEth
    };
    struct Frame
    {
        long long R = 0, llSpan = 1, llUsPP = 1;
        float fNotesY = 0, fNotesCY = 1;
        unsigned uClean = 0;            // Clean flags (Scene.h)
        float fDeflate = 1;             // the inner rect's inset, as Scene::GetNoteKey
        int iRowLo = 0, iRowHi = 0;     // composited rows
        int iThetaLo = 0;
        std::vector< long long > vTheta; // min d with Beta(d) <= b, for b = iThetaLo ...
        std::vector< long long > vG;     // min D with Height(D) >= h
        std::vector< uint64_t > vURow;   // first unique tick with time >= R + Theta(b)
        long long Theta( int b ) const { return vTheta[b - iThetaLo]; }
        uint64_t URow( int b ) const { return vURow[b - iThetaLo]; }
    };

    long long Time( uint64_t u ) const { return (long long)m_Times.Get( u ); }
    uint64_t LowerBoundTime( long long t ) const;
    uint64_t UpperBoundTime( long long t ) const;
    void GetBounds( const VisualStore &S, int k, Bounds &b ) const;
    bool Visible( const VisualStore &S, const uint64_t *pCpos, int k, size_t pos, const NoteView &n, const Bounds &b ) const;
    void CposFor( const VisualStore &S, uint64_t *pCpos ) const;
    void PrepareFrame( const FrameGlobals &g );
    int Beta( long long d ) const;
    int Height( long long D ) const;
    // iBottom: Beta( s - R ) when the caller knows it (the bucket's row), which is the rounded y it
    // would compute (INT_MIN: computed)
    bool Coverage( long long s, long long e, int &a, int &b, int iBottom = INT_MIN ) const;
    void SweepKey( const VisualStore &S, const uint64_t *pCpos, int k, const Bounds &bd, const std::vector< TrackSettings > &vTracks, bool bDegenerate,
                   std::vector< Owner > &vOwners, std::vector< uint64_t > &vRows, size_t &iQueries ) const;
    void ComputeJumpCutoff();

    const Song &m_Song;
    const VisualStore &m_Store;
    const PackedMonotone &m_Times;
    int m_iThreads;

    long long m_llJump = LLONG_MIN;
    bool m_bResumed = false;
    long long m_llProcessed = LLONG_MIN;
    long long m_llEndHW = LLONG_MIN;
    uint64_t m_aCpos[256] = {};         // jump set lower bound per key (the original's backward walk, 6.4)
    // The walk cutoff, independent of the store: 0 every note, 1 no note, 2 from (start, rank) on
    int m_iCut = 0;
    uint64_t m_llCutU = 0;
    uint64_t m_llCutOrder = 0;      // OrderKey of the cutoff note
    unsigned m_uJumpGen = 0;

    // Hidden-channel draw view
    std::vector< uint8_t > m_vMask;                 // current hidden slots
    std::shared_ptr< const VisualStore > m_pView;   // built for m_vViewMask
    std::vector< uint8_t > m_vViewMask;
    uint64_t m_aViewCpos[256] = {};
    unsigned m_uViewCposGen = ~0u;              // jump the view's positions were computed for
    unsigned m_uViewGen = 0;                    // bumped whenever m_pView changes
    unsigned m_uViewCposView = ~0u;             // the view generation they were computed in
    std::thread m_Builder;
    std::atomic< bool > m_bCancelBuild{ false };
    std::vector< uint8_t > m_vBuildMask;
    mutable std::mutex m_mxReady;
    std::shared_ptr< const VisualStore > m_pReady;
    std::vector< uint8_t > m_vReadyMask;
    std::vector< uint8_t > m_vDrawMask;             // the mask the last frame was drawn with

    // LOD cache
    std::unique_ptr< LodCache > m_pLod;
    bool m_bLod = false, m_bLodSync = false, m_bLodFrame = false;
    LodCache::View m_LodView;                       // this frame's ready segments
    LodCache::Setting m_LodSetting;
    bool m_bSkipHidden = false;                     // this frame skips hidden notes (Full store, view pending)

    Frame m_F;
    float m_fCacheY = -1e30f, m_fCacheCY = -1e30f;
    long long m_llCacheSpan = -1;
    int m_iCacheLo = 0, m_iCacheHi = 0;

    std::vector< Owner > m_aOwners[256];
    std::vector< Renderer::SCREEN_VERTEX > m_aVerts[256];   // the owners' rects as quads (Renderer::DrawQuads), in owner order; only grows
    std::vector< Renderer::NoteInstance > m_aInst[256];     // the same as instances (Renderer::InstancingOn); only grows
    size_t m_aVertCount[256] = {};                          // vertices (instances) in use in m_aVerts (m_aInst)
    std::vector< uint32_t > m_aVertStart[256];              // first vertex (instance) of each owner (+ end)
    FrameStats m_Stats;
    bool m_bPixelStats = false;
    bool m_bMergeRows = true;
    const Effects::NoteLight *m_pLight = nullptr;
    std::vector< Renderer::GlowRect > m_aGlow[256], m_vGlow;   // the playing notes' glow rects, by key, then all

    // DrawNotes' working vectors, kept between frames so a frame allocates nothing
    std::vector< KeyCols > m_aClass[Scene::KeyLayers];
    std::vector< int > m_vActive;
    std::vector< size_t > m_vQueries;
    std::vector< long long > m_vSweepUs, m_vVertUs;
    std::vector< double > m_vPixels;
    std::vector< std::vector< uint64_t > > m_vScratch;
    std::vector< MergeItem > m_vMerge;
    std::vector< size_t > m_vMergedCount;
};
