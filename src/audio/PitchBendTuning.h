#pragma once

#include <array>
#include <deque>
#include <map>

#include "audio/MidiOutput.h"
#include "audio/PitchBendRange.h"
#include "audio/Tuning31.h"

class PitchBendTunedOutput : public MidiOutput
{
public:
    explicit PitchBendTunedOutput( std::unique_ptr< MidiOutput > output ) : m_Output( std::move( output ) ) {}
    ~PitchBendTunedOutput() override
    {
        m_Output->AllNotesOff();
        for ( unsigned channel = 0; channel < 16; channel++ )
            if ( m_Slots[channel].group >= 0 )
            {
                CC( channel, 120, 0 );
                CC( channel, 66, 0 );
                SetRange( channel, m_Parts[m_Slots[channel].group].bend.Range() );
                m_Output->Send( 0xE0 | channel | ( 64 << 16 ) );
            }
    }
    void Send( uint32_t message ) override
    {
        unsigned type = message & 0xF0, channel = message & 15;
        if ( channel == 9 || type >= 0xF0 )
        {
            if ( channel != 9 || type < 0x80 || type > 0xA0 || !( message & 0x8000 ) ) m_Output->Send( message );
            return;
        }
        unsigned group = Tuning31::GroupBase( channel );
        unsigned key = ( message >> 8 ) & 255, value = ( message >> 16 ) & 127;
        if ( type == 0x90 && value )
        {
            auto pitch = Tuning31::Pitch( key, channel );
            if ( pitch.second > 127 ) return;               // past the MIDI keys: not played (its off finds no route)
            unsigned lane = pitch.first - group;
            const auto &scale = Tuning31::ScaleMessages[lane];
            unsigned pc = pitch.second % 12;
            int offset = ( ( scale[8 + pc * 2] << 7 ) | scale[9 + pc * 2] ) - 8192;
            unsigned output = Allocate( group, offset );
            auto &slot = m_Slots[output];
            slot.held++;
            slot.age = ++m_Age;
            auto &routes = m_Routes[channel * 256 + key];
            if ( !routes ) routes = std::make_unique< std::deque< Route > >();
            if ( !routes->empty() && routes->back().channel == output && routes->back().generation == slot.generation && routes->back().key == pitch.second )
                routes->back().count++;
            else routes->push_back( { output, slot.generation, pitch.second, 1 } );
            m_Output->Send( 0x90 | output | ( unsigned( pitch.second ) << 8 ) | ( value << 16 ) );
            return;
        }
        if ( type == 0x80 || type == 0x90 || type == 0xA0 )
        {
            auto &routes = m_Routes[channel * 256 + key];
            if ( !routes || routes->empty() ) return;
            if ( type == 0xA0 )
            {
                for ( const auto &route : *routes )
                    if ( route.generation == m_Slots[route.channel].generation )
                        m_Output->Send( 0xA0 | route.channel | ( route.key << 8 ) | ( value << 16 ) );
                return;
            }
            Route route = routes->front();
            if ( !--routes->front().count ) routes->pop_front();
            auto &slot = m_Slots[route.channel];
            if ( route.generation != slot.generation ) return;
            m_Output->Send( type | route.channel | ( route.key << 8 ) | ( value << 16 ) );
            if ( slot.held ) slot.held--;
            slot.sustained |= Pedal( group );
            return;
        }
        auto &part = m_Parts[group];
        if ( type == 0xE0 )
        {
            part.bend.wheel = ( key & 127 ) | ( value << 7 );
            ForGroup( group, [&]( unsigned output ) { Bend( output ); } );
            return;
        }
        if ( type == 0xC0 ) part.program = key & 127;
        if ( type == 0xD0 ) part.pressure = key & 127;
        bool rangeChanged = false;
        if ( type == 0xB0 )
        {
            key &= 127;
            rangeChanged = part.bend.Control( key, value );
            if ( key < 120 && key != 6 && key != 38 && ( key < 96 || key > 101 ) ) part.cc[key] = value;
            RememberParameter( part, key, value );
            if ( key == 121 )
            {
                for ( unsigned controller : { 1u, 11u, 64u, 65u, 66u, 67u, 68u, 69u } ) part.cc[controller] = controller == 11 ? 127 : 0;
                part.pressure = 0;
            }
        }
        ForGroup( group, [&]( unsigned output ) {
            auto &slot = m_Slots[output];
            if ( !rangeChanged || key == 121 ) m_Output->Send( ( message & ~15u ) | output );
            if ( rangeChanged ) { SetRange( output, part.bend.OutputRange() ); Bend( output ); }
            if ( type == 0xB0 && ( key == 120 || key == 123 ) )
            {
                slot.generation++;
                slot.held = 0;
                slot.sustained = key == 123 && Pedal( group );
            }
            if ( type == 0xB0 && ( key == 64 || key == 66 || key == 121 ) && !Pedal( group ) ) slot.sustained = false;
        } );
        if ( type == 0xB0 && ( key == 120 || key == 123 ) )
            for ( unsigned source = group; source < group + 3; source++ )
                for ( unsigned note = 0; note < 256; note++ ) m_Routes[source * 256 + note].reset();
    }
    void ResetPlaybackState() override
    {
        m_Output->AllNotesOff();
        for ( unsigned channel = 0; channel < 16; channel++ )
            if ( m_Slots[channel].group >= 0 )
            {
                CC( channel, 120, 0 );
                CC( channel, 66, 0 );
                SetRange( channel, 2.0 );
                m_Output->Send( 0xE0 | channel | ( 64 << 16 ) );
                m_Slots[channel] = Slot();
            }
        for ( auto &part : m_Parts ) part = Part();
        for ( auto &routes : m_Routes ) routes.reset();
        m_Age = 0;
    }
    void SetVolume( double volume ) override { m_Output->SetVolume( volume ); }
    const std::wstring &Name() const override { return m_Output->Name(); }
    bool IsKdmapi() const override { return m_Output->IsKdmapi(); }

private:
    struct Part
    {
        PitchBendRange bend;
        std::array< int, 120 > cc;
        std::map< unsigned, int > parameters{ { 1, 8192 }, { 2, 8192 } };
        unsigned program = 0, pressure = 0;
        Part()
        {
            cc.fill( -1 );
            cc[0] = cc[32] = cc[64] = cc[66] = 0;
            cc[7] = 100;
            cc[10] = 64;
            cc[11] = 127;
        }
    };
    struct Slot
    {
        int group = -1, offset = 0;
        uint64_t generation = 0, age = 0;
        uint64_t held = 0;
        bool sustained = false;
    };
    struct Route { unsigned channel; uint64_t generation; unsigned key; uint64_t count; };

    bool Pedal( unsigned group ) const { return m_Parts[group].cc[64] >= 64 || m_Parts[group].cc[66] >= 64; }
    template< class F > void ForGroup( unsigned group, F fn )
    {
        for ( unsigned channel = 0; channel < 16; channel++ )
            if ( m_Slots[channel].group == (int)group ) fn( channel );
    }
    void CC( unsigned channel, unsigned controller, unsigned value )
    {
        m_Output->Send( 0xB0 | channel | ( controller << 8 ) | ( value << 16 ) );
    }
    void Select( unsigned channel, unsigned parameter )
    {
        CC( channel, parameter & 0x4000 ? 99 : 101, ( parameter >> 7 ) & 127 );
        CC( channel, parameter & 0x4000 ? 98 : 100, parameter & 127 );
    }
    void RestoreSelection( unsigned channel )
    {
        const auto &state = m_Parts[m_Slots[channel].group].bend;
        Select( channel, state.nrpn ? 0x4000 | ( state.nrpnMSB << 7 ) | state.nrpnLSB : ( state.rpnMSB << 7 ) | state.rpnLSB );
    }
    void SetRange( unsigned channel, double range )
    {
        Select( channel, 0 );
        unsigned value = (unsigned)std::lround( range * 100.0 );
        CC( channel, 6, value / 100 );
        CC( channel, 38, value % 100 );
        RestoreSelection( channel );
    }
    void Bend( unsigned channel )
    {
        const auto &slot = m_Slots[channel];
        int wheel = m_Parts[slot.group].bend.Bend( slot.offset * 100.0 / 8192.0 );
        m_Output->Send( 0xE0 | channel | ( ( wheel & 127 ) << 8 ) | ( ( wheel >> 7 ) << 16 ) );
    }
    void RememberParameter( Part &part, unsigned controller, unsigned value )
    {
        const auto &state = part.bend;
        unsigned parameter = state.nrpn ? 0x4000 | ( state.nrpnMSB << 7 ) | state.nrpnLSB : ( state.rpnMSB << 7 ) | state.rpnLSB;
        if ( ( parameter & 0x3FFF ) == 0x3FFF || parameter == 0 ) return;
        if ( controller != 6 && controller != 38 && controller != 96 && controller != 97 ) return;
        int &data = part.parameters[parameter];
        if ( controller == 6 ) data = ( value << 7 ) | ( data & 127 );
        if ( controller == 38 ) data = ( data & 0x3F80 ) | value;
        int step = parameter == 2 ? 128 : 1;
        if ( controller == 96 ) data = std::min( 16383, data + step );
        if ( controller == 97 ) data = std::max( 0, data - step );
    }
    unsigned Allocate( unsigned group, int offset )
    {
        for ( unsigned channel = 0; channel < 16; channel++ )
            if ( channel != 9 && m_Slots[channel].group == (int)group && m_Slots[channel].offset == offset ) return channel;
        unsigned output = 16;
        for ( unsigned channel = 0; channel < 16; channel++ )
            if ( channel != 9 && !m_Slots[channel].held && !m_Slots[channel].sustained )
            {
                if ( m_Slots[channel].group < 0 ) { output = channel; break; }
                if ( output == 16 || m_Slots[channel].age < m_Slots[output].age ) output = channel;
            }
        if ( output == 16 )
            for ( unsigned channel = 0; channel < 16; channel++ )
                if ( channel != 9 && ( output == 16 || m_Slots[channel].age < m_Slots[output].age ) ) output = channel;
        CC( output, 120, 0 );
        CC( output, 121, 0 );
        auto &slot = m_Slots[output];
        slot.group = group;
        slot.offset = offset;
        slot.held = 0;
        slot.sustained = false;
        slot.generation++;
        auto &part = m_Parts[group];
        for ( unsigned controller = 0; controller < part.cc.size(); controller++ )
            if ( part.cc[controller] >= 0 ) CC( output, controller, part.cc[controller] );
        m_Output->Send( 0xC0 | output | ( part.program << 8 ) );
        m_Output->Send( 0xD0 | output | ( part.pressure << 8 ) );
        for ( const auto &[parameter, data] : part.parameters )
        {
            Select( output, parameter );
            CC( output, 6, data >> 7 );
            CC( output, 38, data & 127 );
        }
        SetRange( output, part.bend.OutputRange() );
        Bend( output );
        return output;
    }

    std::unique_ptr< MidiOutput > m_Output;
    Part m_Parts[16];
    Slot m_Slots[16];
    std::unique_ptr< std::deque< Route > > m_Routes[4096];
    uint64_t m_Age = 0;
};
