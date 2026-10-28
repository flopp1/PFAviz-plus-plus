#include <cassert>
#include <cstdio>
#include <cstdlib>
#include "audio/MidiOutput.cpp"

Config &Config::GetConfig() { std::abort(); }

struct Capture
{
    std::vector< uint32_t > notes;
    std::vector< std::vector< uint8_t > > sysex;
};

class CaptureOutput : public MidiOutput
{
public:
    explicit CaptureOutput( Capture &capture ) : m_Capture( capture ) {}
    void Send( uint32_t message ) override { m_Capture.notes.push_back( message ); }
    bool SendSysEx( const uint8_t *data, size_t n ) override
    {
        m_Capture.sysex.emplace_back( data, data + n );
        return true;
    }
    const std::wstring &Name() const override { static const std::wstring name = L"Capture"; return name; }
private:
    Capture &m_Capture;
};

static uint32_t Note( unsigned type, unsigned channel, unsigned key, unsigned value = 100 )
{
    return type | channel | ( key << 8 ) | ( value << 16 );
}

int main()
{
    Capture capture;
    {
        TunedOutput output( std::make_unique< CaptureOutput >( capture ) );
        assert( output.Init() && capture.sysex.size() == 3 );
        const unsigned cases[][4] = {
            { 0, 124, 0, 48 }, { 0, 125, 1, 48 }, { 0, 127, 1, 49 }, { 0, 128, 2, 50 },
            { 1, 186, 0, 96 }, { 2, 186, 0, 120 }, { 10, 128, 12, 50 }, { 15, 186, 13, 120 },
            { 0, 255, 0, 99 }, { 9, 60, 9, 60 }
        };
        for ( const auto &c : cases )
        {
            const uint32_t input[] = { Note( 0x90, c[0], c[1] ), Note( 0xA0, c[0], c[1], 70 ),
                                       Note( 0x80, c[0], c[1], 0 ), Note( 0x90, c[0], c[1], 0 ) };
            const std::vector< uint32_t > expected = { Note( 0x90, c[2], c[3] ), Note( 0xA0, c[2], c[3], 70 ),
                                                      Note( 0x80, c[2], c[3], 0 ), Note( 0x90, c[2], c[3], 0 ) };
            capture.notes.clear();
            output.SendBatch( input, 4 );
            assert( capture.notes == expected );
        }
        capture.notes.clear();
        output.Send( Note( 0x90, 9, 128 ) );
        assert( capture.notes.empty() );
        output.Send( Note( 0x90, 15, 255 ) );           // key 147: past the MIDI keys, dropped
        assert( capture.notes.empty() );
        for ( unsigned type : { 0xB0u, 0xC0u, 0xD0u, 0xE0u } )
        {
            capture.notes.clear();
            output.Send( Note( type, 11, 64, type == 0xC0 || type == 0xD0 ? 0 : 90 ) );
            assert( capture.notes.size() == 3 );
            for ( unsigned lane = 0; lane < 3; lane++ ) assert( ( capture.notes[lane] & 15 ) == 10 + lane );
            capture.notes.clear();
            output.Send( Note( type, 9, 64, 0 ) );
            assert( capture.notes.size() == 1 && ( capture.notes[0] & 15 ) == 9 );
        }
    }
    assert( capture.sysex.size() == 6 );
    for ( size_t lane = 3; lane < 6; lane++ )
        for ( size_t i = 8; i < 32; i += 2 )
            assert( capture.sysex[lane][i] == 0x40 && capture.sysex[lane][i + 1] == 0 );
    puts( "31-EDO routing, 256-key positions, controller lanes, drums, and tuning cleanup passed." );
}
