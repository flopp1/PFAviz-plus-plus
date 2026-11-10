// The 31-EDO output by scale tuning (Performance tab): each step of 31 becomes a key on one of a channel
// group's three channels, which the MIDI Tuning Standard's scale/octave messages (sent once, at Init)
// set apart; channel messages go to all three. Drums are left alone; keys past 127 are not played
#pragma once

#include <algorithm>
#include <memory>

#include "audio/MidiOutput.h"
#include "audio/Tuning31.h"

class ScaleTunedOutput : public MidiOutput
{
public:
    explicit ScaleTunedOutput( std::unique_ptr< MidiOutput > p ) : m_pOut( std::move( p ) ) {}
    ~ScaleTunedOutput() override
    {
        m_pOut->AllNotesOff();
        for ( const auto &message : Tuning31::ScaleMessages )
        {
            uint8_t neutral[33];
            std::copy( message, message + 33, neutral );
            for ( size_t i = 8; i < 32; i += 2 ) { neutral[i] = 0x40; neutral[i + 1] = 0; }
            m_pOut->SendSysEx( neutral, sizeof( neutral ) );
        }
    }
    bool Init()
    {
        for ( const auto &message : Tuning31::ScaleMessages )
            if ( !m_pOut->SendSysEx( message, sizeof( message ) ) ) return false;
        return true;
    }
    void Send( uint32_t message ) override
    {
        unsigned type = message & 0xF0, channel = message & 0x0F;
        if ( type == 0x80 || type == 0x90 || type == 0xA0 )
        {
            auto pitch = Tuning31::Pitch( ( message >> 8 ) & 0xFF, channel );
            if ( pitch.second < 128 ) m_Batch.Emit( ( message & ~0xFF0Fu ) | pitch.first | ( uint32_t( pitch.second ) << 8 ) );
        }
        else if ( channel != 9 && type >= 0xB0 && type <= 0xE0 )
        {
            unsigned base = Tuning31::GroupBase( channel );
            for ( unsigned lane = 0; lane < 3; lane++ ) m_Batch.Emit( ( message & ~0x0Fu ) | ( base + lane ) );
        }
        else m_Batch.Emit( message );
    }
    void SendBatch( const uint32_t *pMsgs, size_t n ) override { m_Batch.Run( pMsgs, n, [&]( uint32_t m ) { Send( m ); } ); }
    void Flush() override { m_pOut->Flush(); }
    void SetVolume( double volume ) override { m_pOut->SetVolume( volume ); }
    const std::wstring &Name() const override { return m_pOut->Name(); }
    bool IsKdmapi() const override { return m_pOut->IsKdmapi(); }

private:
    std::unique_ptr< MidiOutput > m_pOut;
    BatchOut m_Batch{ m_pOut };
};
