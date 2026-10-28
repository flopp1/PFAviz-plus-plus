#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "audio/MidiOutput.cpp"

Config &Config::GetConfig() { std::abort(); }

struct Synth
{
    struct State { int semitones = 2, cents = 0, wheel = 8192, msb = 127, lsb = 127, program = 0, volume = 100; } channels[16];
    struct Event { unsigned channel, key; };
    std::vector< Event > ons, offs;
    unsigned sysex = 0, steals = 0;
    double Pitch( const Event &event ) const
    {
        const auto &state = channels[event.channel];
        return event.key + ( state.wheel - 8192 ) * ( state.semitones + state.cents / 100.0 ) / 8192.0;
    }
};

class TestOutput : public MidiOutput
{
public:
    explicit TestOutput( Synth &synth ) : m_Synth( synth ) {}
    void Send( uint32_t message ) override
    {
        unsigned channel = message & 15, type = message & 0xF0, key = ( message >> 8 ) & 127, value = ( message >> 16 ) & 127;
        auto &state = m_Synth.channels[channel];
        if ( type == 0x90 && value ) m_Synth.ons.push_back( { channel, key } );
        if ( type == 0x80 || ( type == 0x90 && !value ) ) m_Synth.offs.push_back( { channel, key } );
        if ( type == 0xC0 ) state.program = key;
        if ( type == 0xE0 ) state.wheel = key | ( value << 7 );
        if ( type == 0xB0 )
        {
            if ( key == 101 ) state.msb = value;
            if ( key == 100 ) state.lsb = value;
            if ( key == 99 || key == 98 ) state.msb = state.lsb = 127;
            if ( key == 7 ) state.volume = value;
            if ( key == 120 ) m_Synth.steals++;
            if ( key == 121 ) { state.wheel = 8192; state.msb = state.lsb = 127; }
            if ( !state.msb && !state.lsb )
            {
                if ( key == 6 ) state.semitones = value;
                if ( key == 38 ) state.cents = value;
            }
        }
    }
    bool SendSysEx( const uint8_t *, size_t ) override { m_Synth.sysex++; return false; }
    const std::wstring &Name() const override { static const std::wstring name = L"Test"; return name; }
private:
    Synth &m_Synth;
};

static uint32_t Note( unsigned channel, unsigned key, unsigned velocity = 100 )
{
    return 0x90 | channel | ( key << 8 ) | ( velocity << 16 );
}
static void CC( MidiOutput &output, unsigned channel, unsigned controller, unsigned value )
{
    output.Send( 0xB0 | channel | ( controller << 8 ) | ( value << 16 ) );
}
static double Expected( unsigned channel, unsigned key )
{
    auto pitch = Tuning31::Pitch( key, channel );
    const auto &scale = Tuning31::ScaleMessages[pitch.first - Tuning31::GroupBase( channel )];
    unsigned pc = pitch.second % 12;
    return pitch.second + ( ( ( scale[8 + pc * 2] << 7 ) | scale[9 + pc * 2] ) - 8192 ) / 8192.0;
}
static void Near( double actual, double expected, double tolerance = 0.0003 )
{
    if ( std::abs( actual - expected ) > tolerance ) std::printf( "pitch %.9f expected %.9f\n", actual, expected );
    assert( std::abs( actual - expected ) <= tolerance );
}

int main()
{
    Synth synth;
    {
        PitchBendTunedOutput output( std::make_unique< TestOutput >( synth ) );
        for ( unsigned channel = 0; channel < 16; channel++ )
            if ( channel != 9 )
                for ( unsigned key = 0; key < 256; key++ )
                {
                    if ( Tuning31::Pitch( key, channel ).second > 127 )
                    {
                        // Past the MIDI keys: neither the note nor its off is sent
                        size_t ons = synth.ons.size(), offs = synth.offs.size();
                        output.Send( Note( channel, key ) );
                        output.Send( Note( channel, key, 0 ) );
                        assert( synth.ons.size() == ons && synth.offs.size() == offs );
                        continue;
                    }
                    output.Send( Note( channel, key ) );
                    assert( synth.ons.back().channel != 9 );
                    Near( synth.Pitch( synth.ons.back() ), Expected( channel, key ) );
                    output.Send( Note( channel, key, 0 ) );
                    assert( synth.offs.back().channel == synth.ons.back().channel && synth.offs.back().key == synth.ons.back().key );
                }
        output.Send( Note( 9, 60 ) );
        assert( synth.ons.back().channel == 9 && synth.ons.back().key == 60 );
        size_t count = synth.ons.size();
        output.Send( Note( 9, 128 ) );
        assert( synth.ons.size() == count );
    }
    assert( synth.sysex == 0 );
    for ( const auto &state : synth.channels ) assert( state.wheel == 8192 );
    Synth chord;
    {
        PitchBendTunedOutput output( std::make_unique< TestOutput >( chord ) );
        output.Send( Note( 0, 124 ) );
        output.Send( Note( 0, 126 ) );
        assert( chord.ons[0].channel != chord.ons[1].channel );
        Near( chord.Pitch( chord.ons[0] ), Expected( 0, 124 ) );
        Near( chord.Pitch( chord.ons[1] ), Expected( 0, 126 ) );
        output.Send( Note( 0, 155 ) );
        assert( chord.ons[2].channel == chord.ons[0].channel );
        for ( int range : { 0, 2, 12, 48, 96 } )
        {
            CC( output, 0, 101, 0 ); CC( output, 0, 100, 0 );
            CC( output, 0, 6, range ); CC( output, 0, 38, 50 );
            for ( int wheel : { 0, 8192, 16383 } )
            {
                output.Send( 0xE0 | ( ( wheel & 127 ) << 8 ) | ( ( wheel >> 7 ) << 16 ) );
                for ( size_t note = 0; note < chord.ons.size(); note++ )
                {
                    unsigned key = note == 0 ? 124 : note == 1 ? 126 : 155;
                    Near( chord.Pitch( chord.ons[note] ), Expected( 0, key ) + ( wheel - 8192 ) * ( range + 0.5 ) / 8192.0, ( range + 1.5 ) / 16384.0 + 0.000001 );
                }
            }
        }
        CC( output, 0, 121, 0 );
        Near( chord.Pitch( chord.ons[1] ), Expected( 0, 126 ), 98.0 / 16384 );
        CC( output, 0, 101, 0 ); CC( output, 0, 100, 0 ); CC( output, 0, 6, 48 ); CC( output, 0, 38, 0 );
        CC( output, 0, 99, 1 ); CC( output, 0, 98, 2 ); CC( output, 0, 6, 12 );
        output.Send( 0xE0 );
        Near( chord.Pitch( chord.ons[1] ), Expected( 0, 126 ) - 48, 49.0 / 16384 );
        output.Send( 0xC0 | ( 42 << 8 ) ); CC( output, 0, 7, 90 );
        output.Send( Note( 0, 128 ) );
        assert( chord.channels[chord.ons.back().channel].program == 42 && chord.channels[chord.ons.back().channel].volume == 90 );
        Near( chord.Pitch( chord.ons.back() ), Expected( 0, 128 ) - 48, 49.0 / 16384 );
        output.ResetPlaybackState();
        output.Send( Note( 0, 128 ) );
        Near( chord.Pitch( chord.ons.back() ), Expected( 0, 128 ) );
        assert( chord.channels[chord.ons.back().channel].program == 0 && chord.channels[chord.ons.back().channel].volume == 100 );
    }
    Synth overflow;
    {
        PitchBendTunedOutput output( std::make_unique< TestOutput >( overflow ) );
        for ( unsigned key = 0; key < 16; key++ ) output.Send( Note( 0, key ) );
        assert( overflow.ons.size() == 16 );
        for ( size_t i = 0; i < 15; i++ )
            for ( size_t j = i + 1; j < 15; j++ ) assert( overflow.ons[i].channel != overflow.ons[j].channel );
        assert( overflow.ons[15].channel == overflow.ons[0].channel );
        output.Send( Note( 0, 0, 0 ) );
        assert( overflow.offs.empty() );
        output.Send( Note( 0, 15, 0 ) );
        assert( overflow.offs.size() == 1 && overflow.offs.back().key == overflow.ons[15].key );
    }
    Synth sustain;
    {
        PitchBendTunedOutput output( std::make_unique< TestOutput >( sustain ) );
        CC( output, 0, 64, 127 );
        output.Send( Note( 0, 0 ) ); output.Send( Note( 0, 0, 0 ) );
        output.Send( Note( 0, 1 ) );
        assert( sustain.ons[0].channel != sustain.ons[1].channel );
        Near( sustain.Pitch( sustain.ons[0] ), Expected( 0, 0 ) );
        CC( output, 0, 64, 0 );
        output.AllNotesOff();
        size_t offs = sustain.offs.size();
        output.Send( Note( 0, 1, 0 ) );
        assert( sustain.offs.size() == offs );
        output.Send( Note( 0, 1 ) );
        output.Send( Note( 0, 1, 0 ) );
        assert( sustain.offs.size() == offs + 1 );
    }
    puts( "Pitch-bend 31-EDO: all keys and parts, chords, wide bend ranges, controllers, sustain, channel stealing, stale offs, and cleanup passed." );
}
