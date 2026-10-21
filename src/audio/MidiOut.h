// Frame-driven output used by the splash screen (it plays from the game thread, as the original did).
// Opens the configured output: KDMAPI when preferred and available, otherwise the WinMM device.
#pragma once

#include <memory>
#include <string>

#include "audio/MidiOutput.h"

class MidiOut
{
public:
    bool OpenConfigured();
    void Close() { m_pOut.reset(); }
    bool IsOpen() const { return m_pOut != nullptr; }
    const std::wstring &GetDevice() const { static const std::wstring s; return m_pOut ? m_pOut->Name() : s; }

    void AllNotesOff() { if ( m_pOut ) m_pOut->AllNotesOff(); }
    void SetVolume( double dVolume ) { if ( m_pOut ) m_pOut->SetVolume( dVolume ); }
    bool PlayEvent( unsigned char cStatus, unsigned char cParam1, unsigned char cParam2 = 0 )
    {
        if ( !m_pOut ) return false;
        m_pOut->Send( cStatus | ( cParam1 << 8 ) | ( cParam2 << 16 ) );
        return true;
    }

private:
    std::unique_ptr< MidiOutput > m_pOut;
};
