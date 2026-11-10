// RenderAhead::FromConfig: the settings as Preferences keep them (apart, so the test harness can use
// RenderAhead without the configuration)
#include "audio/RenderAhead.h"

#include <cwctype>

#include "core/Config.h"

namespace
{
    std::vector< std::wstring > SplitFonts( const std::wstring &s )
    {
        std::vector< std::wstring > v;
        size_t i = 0;
        while ( i <= s.size() )
        {
            size_t j = s.find( L'|', i );
            if ( j == std::wstring::npos ) j = s.size();
            std::wstring t = s.substr( i, j - i );
            while ( !t.empty() && iswspace( t.back() ) ) t.pop_back();
            size_t a = 0;
            while ( a < t.size() && iswspace( t[a] ) ) a++;
            if ( a < t.size() ) v.push_back( t.substr( a ) );
            i = j + 1;
        }
        return v;
    }
}

RenderAhead::Settings RenderAhead::FromConfig()
{
    AudioSettings a = Config::GetConfig().CopyAudioSettings();
    Settings s;
    s.bOn = a.bRendered;
    s.eEngine = a.eRenderEngine;
    s.vFonts = SplitFonts( a.sSoundfonts );
    s.iBassVoices = a.iBassVoices;
    s.iBassThreads = a.iBassThreads;
    s.iGpuShare = a.iGpuShare;
    s.kb = a.kb;
    s.bLossless = a.bRenderLossless;
    s.bAutoDegrade = a.bRenderDegrade;
    s.bDiscardOnSwitch = a.bRenderDiscard;
    s.iKestrelVoices = a.iKestrelVoices;
    s.iAheadSecs = a.iAheadSecs;
    s.iBehindSecs = a.iBehindSecs;
    s.iBassNps = a.iBassNps;
    s.bBassByChannel = a.bBassByChannel;
    s.bLimiter = a.bLimiter;
    s.bFx = a.bRenderFx;
    return s;
}

void RenderAhead::PreloadFromConfig()
{
    const Settings s = FromConfig();
    if ( !s.bOn || s.vFonts.empty() ) return;
    RenderBackends::BankKey key;
    key.eEngine = s.eEngine;
    key.vFonts = s.vFonts;
    key.uRate = 0;      // (the sound device's, as playback will open it)
    key.bFilter = key.eEngine == RenderBackends::EngineBass || s.kb.bFilter;
    RenderBackends::Preload( key );
}
