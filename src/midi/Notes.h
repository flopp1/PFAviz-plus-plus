// Note geometry helpers, identical to the original MIDI::IsSharp / NoteVal / WhiteCount tables.
#pragma once

namespace Notes
{
    enum Note { A, AS, B, C, CS, D, DS, E, F, FS, G, GS };

    constexpr int KEYS = 257;
    constexpr int C8 = 108;
    constexpr int C4 = C8 - 4 * 12;
    constexpr int A0 = C8 - 7 * 12 - 3;
    constexpr int Drums = 0x09;

    bool IsSharp( int iNote );
    Note NoteVal( int iNote );
    int WhiteCount( int iMinNote, int iMaxNote ); // white keys in [iMinNote, iMaxNote)
    const wchar_t *NoteName( int iNote );

    extern const wchar_t *const Instruments[129];
}
