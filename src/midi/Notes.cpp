#include "midi/Notes.h"

#include <cwchar>

namespace
{
    struct Tables
    {
        Notes::Note aNoteVal[Notes::KEYS];
        bool aIsSharp[Notes::KEYS];
        int aWhiteCount[Notes::KEYS + 1];

        // Same walk as the original MIDI::InitArrays
        Tables()
        {
            bool bIsSharp = false;
            Notes::Note eNote = Notes::C;
            for ( int i = 0; i < Notes::KEYS; i++ )
            {
                aNoteVal[i] = eNote;
                aIsSharp[i] = bIsSharp;
                if ( eNote != Notes::B && eNote != Notes::E )
                    bIsSharp = !bIsSharp;
                if ( eNote == Notes::GS ) eNote = Notes::A;
                else eNote = static_cast< Notes::Note >( eNote + 1 );
            }
            aWhiteCount[0] = 0;
            for ( int i = 1; i < Notes::KEYS + 1; i++ )
                aWhiteCount[i] = aWhiteCount[i - 1] + !aIsSharp[i - 1];
        }
    };
    const Tables &T() { static const Tables t; return t; }
}

bool Notes::IsSharp( int iNote )
{
    if ( iNote < 0 || iNote >= KEYS ) return false;
    return T().aIsSharp[iNote];
}

Notes::Note Notes::NoteVal( int iNote )
{
    if ( iNote < 0 || iNote >= KEYS ) return C;
    return T().aNoteVal[iNote];
}

int Notes::WhiteCount( int iMinNote, int iMaxNote )
{
    if ( iMinNote < 0 || iMinNote >= KEYS || iMaxNote < 0 || iMaxNote >= KEYS ) return 0;
    return T().aWhiteCount[iMaxNote] - T().aWhiteCount[iMinNote];
}

const wchar_t *const Notes::Instruments[129] =
{
    L"Acoustic Grand Piano", L"Bright Acoustic Piano", L"Electric Grand Piano", L"Honky-tonk Piano", L"Electric Piano 1",
    L"Electric Piano 2", L"Harpsichord", L"Clavi", L"Celesta", L"Glockenspiel",
    L"Music Box", L"Vibraphone", L"Marimba", L"Xylophone", L"Tubular Bells",
    L"Dulcimer", L"Drawbar Organ", L"Percussive Organ", L"Rock Organ", L"Church Organ",
    L"Reed Organ", L"Accordion", L"Harmonica", L"Tango Accordion", L"Acoustic Guitar (nylon)",
    L"Acoustic Guitar (steel)", L"Electric Guitar (jazz)", L"Electric Guitar (clean)", L"Electric Guitar (muted)", L"Overdriven Guitar",
    L"Distortion Guitar", L"Guitar harmonics", L"Acoustic Bass", L"Electric Bass (finger)", L"Electric Bass (pick)",
    L"Fretless Bass", L"Slap Bass 1", L"Slap Bass 2", L"Synth Bass 1", L"Synth Bass 2",
    L"Violin", L"Viola", L"Cello", L"Contrabass", L"Tremolo Strings",
    L"Pizzicato Strings", L"Orchestral Harp", L"Timpani", L"String Ensemble 1", L"String Ensemble 2",
    L"SynthStrings 1", L"SynthStrings 2", L"Choir Aahs", L"Voice Oohs", L"Synth Voice",
    L"Orchestra Hit", L"Trumpet", L"Trombone", L"Tuba", L"Muted Trumpet",
    L"French Horn", L"Brass Section", L"SynthBrass 1", L"SynthBrass 2", L"Soprano Sax",
    L"Alto Sax", L"Tenor Sax", L"Baritone Sax", L"Oboe", L"English Horn",
    L"Bassoon", L"Clarinet", L"Piccolo", L"Flute", L"Recorder",
    L"Pan Flute", L"Blown Bottle", L"Shakuhachi", L"Whistle", L"Ocarina",
    L"Lead 1 (square)", L"Lead 2 (sawtooth)", L"Lead 3 (calliope)", L"Lead 4 (chiff)", L"Lead 5 (charang)",
    L"Lead 6 (voice)", L"Lead 7 (fifths)", L"Lead 8 (bass + lead)", L"Pad 1 (new age)", L"Pad 2 (warm)",
    L"Pad 3 (polysynth)", L"Pad 4 (choir)", L"Pad 5 (bowed)", L"Pad 6 (metallic)", L"Pad 7 (halo)",
    L"Pad 8 (sweep)", L"FX 1 (rain)", L"FX 2 (soundtrack)", L"FX 3 (crystal)", L"FX 4 (atmosphere)",
    L"FX 5 (brightness)", L"FX 6 (goblins)", L"FX 7 (echoes)", L"FX 8 (sci-fi)", L"Sitar",
    L"Banjo", L"Shamisen", L"Koto", L"Kalimba", L"Bag pipe",
    L"Fiddle", L"Shanai", L"Tinkle Bell", L"Agogo", L"Steel Drums",
    L"Woodblock", L"Taiko Drum", L"Melodic Tom", L"Synth Drum", L"Reverse Cymbal",
    L"Guitar Fret Noise", L"Breath Noise", L"Seashore", L"Bird Tweet", L"Telephone Ring",
    L"Helicopter", L"Applause", L"Gunshot", L"Various"
};

const wchar_t *Notes::NoteName( int iNote )
{
    static wchar_t aNames[128][6];
    static bool bInit = false;
    if ( !bInit )
    {
        static const wchar_t *const aLetters[12] = { L"C", L"C#", L"D", L"D#", L"E", L"F", L"F#", L"G", L"G#", L"A", L"A#", L"B" };
        for ( int i = 0; i < 128; i++ )
        {
            int iOctave = i / 12 - 1;
            swprintf( aNames[i], 6, L"%ls%ls%d", aLetters[i % 12], iOctave < 0 ? L"-" : L"", iOctave < 0 ? -iOctave : iOctave );
        }
        bInit = true;
    }
    return iNote >= 0 && iNote < 128 ? aNames[iNote] : L"";
}
