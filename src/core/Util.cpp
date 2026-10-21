#include "core/Util.h"

#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

static long long QpcFrequency()
{
    static long long llFreq = [] { LARGE_INTEGER li; QueryPerformanceFrequency( &li ); return li.QuadPart; }();
    return llFreq;
}

long long Timer::NowMicroSecs()
{
    LARGE_INTEGER li;
    QueryPerformanceCounter( &li );
    long long llFreq = QpcFrequency();
    // Split to avoid overflow: whole seconds plus remainder.
    return ( li.QuadPart / llFreq ) * 1000000LL + ( ( li.QuadPart % llFreq ) * 1000000LL ) / llFreq;
}

Timer::Timer() {}

void Timer::Start()
{
    m_bStarted = true;
    m_llStart = NowMicroSecs();
}

long long Timer::GetMicroSecs() const
{
    return m_bStarted ? NowMicroSecs() - m_llStart : 0;
}

uint64_t Util::ProcessMemory()
{
    // PROCESS_MEMORY_COUNTERS_EX2 (Windows 10 1809), not in the MinGW headers
    struct Counters
    {
        DWORD cb, PageFaultCount;
        SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage,
               QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage, PrivateWorkingSetSize;
        ULONG64 SharedCommitUsage;
    } c = {};
    c.cb = sizeof( c );
    if ( K32GetProcessMemoryInfo( GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&c, sizeof( c ) ) && c.PrivateWorkingSetSize )
        return c.PrivateWorkingSetSize;
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    pmc.cb = sizeof( pmc );
    return K32GetProcessMemoryInfo( GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof( pmc ) ) ? pmc.PrivateUsage : 0;
}

std::wstring Util::Utf8ToWide( const std::string &s )
{
    if ( s.empty() ) return std::wstring();
    int n = MultiByteToWideChar( CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0 );
    std::wstring w( n, L'\0' );
    MultiByteToWideChar( CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n );
    return w;
}

std::string Util::WideToUtf8( const std::wstring &w )
{
    if ( w.empty() ) return std::string();
    int n = WideCharToMultiByte( CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr );
    std::string s( n, '\0' );
    WideCharToMultiByte( CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr );
    return s;
}

unsigned Util::RandColor()
{
    int R, G, B;
    HSVtoRGB( rand() % 360, rand() % 40 + 60, rand() % 20 + 80, R, G, B );
    return ( B << 16 ) | ( G << 8 ) | R;
}

void Util::RGBtoHSV( int R, int G, int B, int &H, int &S, int &V )
{
    double dR = R / 255.0, dG = G / 255.0, dB = B / 255.0;
    double M = std::max( std::max( dR, dG ), dB );
    double m = std::min( std::min( dR, dG ), dB );
    double C = M - m;

    double dH = C ==  0 ? 0.0 :
                M == dR ? ( dG - dB ) / C :
                M == dG ? ( dB - dR ) / C + 2 :
                M == dB ? ( dR - dG ) / C + 4 : 0.0;
    if ( dH < 0 ) dH += 6.0;
    double dV = M;
    double dS = ( dV > 0.0 ? C / dV : 0.0 );

    H = static_cast< int >( dH * 60.0 + 0.5 ) % 360;
    V = static_cast< int >( dV * 100.0 + 0.5 );
    S = static_cast< int >( dS * 100.0 + 0.5 );
}

void Util::HSVtoRGB( int H, int S, int V, int &R, int &G, int &B )
{
    double dH = H / 60.0, dS = S / 100.0, dV = V / 100.0;
    double C = dV * dS;
    double m = dV - C;

    double dR1 = 0.0, dG1 = 0.0, dB1 = 0.0;
    if ( dH >= 0 && dH < 1.0 ) { dR1 = C; dG1 = C * dH; dB1 = 0; }
    if ( dH >= 1 && dH < 2.0 ) { dR1 = C * ( 2.0 - dH ); dG1 = C; dB1 = 0; }
    if ( dH >= 2 && dH < 3.0 ) { dR1 = 0; dG1 = C; dB1 = C * ( dH - 2.0 ); }
    if ( dH >= 3 && dH < 4.0 ) { dR1 = 0; dG1 = C * ( 4.0 - dH ); dB1 = C; }
    if ( dH >= 4 && dH < 5.0 ) { dR1 = C * ( dH - 4.0 ); dG1 = 0; dB1 = C; }
    if ( dH >= 5 && dH < 6.0 ) { dR1 = C; dG1 = 0; dB1 = C * ( 6.0 - dH ); }

    R = static_cast< int >( ( dR1 + m ) * 255.0 + 0.5 );
    G = static_cast< int >( ( dG1 + m ) * 255.0 + 0.5 );
    B = static_cast< int >( ( dB1 + m ) * 255.0 + 0.5 );
}

void Util::CommaPrintf( wchar_t buf[32], long long iVal )
{
    long long iAbsVal = iVal < 0 ? -iVal : iVal;
    if ( iAbsVal < 1000 ) swprintf( buf, 32, L"%lld", iVal );
    else if ( iAbsVal < 1000000 ) swprintf( buf, 32, L"%lld,%03lld", iVal / 1000, iAbsVal % 1000 );
    else if ( iAbsVal < 1000000000 ) swprintf( buf, 32, L"%lld,%03lld,%03lld", iVal / 1000000, ( iAbsVal / 1000 ) % 1000, iAbsVal % 1000 );
    else swprintf( buf, 32, L"%lld,%03lld,%03lld,%03lld", iVal / 1000000000, ( iAbsVal / 1000000 ) % 1000, ( iAbsVal / 1000 ) % 1000, iAbsVal % 1000 );
}
