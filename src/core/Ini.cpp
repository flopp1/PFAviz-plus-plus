#include "core/Ini.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

static std::string Trim( const std::string &s )
{
    size_t a = s.find_first_not_of( " \t\r\n" );
    if ( a == std::string::npos ) return std::string();
    size_t b = s.find_last_not_of( " \t\r\n" );
    return s.substr( a, b - a + 1 );
}

bool IniFile::Load( const std::wstring &sPath )
{
    m_mData.clear();
    FILE *f = _wfopen( sPath.c_str(), L"rb" );
    if ( !f ) return false;
    std::string sAll;
    char buf[4096];
    size_t n;
    while ( ( n = fread( buf, 1, sizeof( buf ), f ) ) > 0 ) sAll.append( buf, n );
    fclose( f );
    if ( sAll.size() >= 3 && (unsigned char)sAll[0] == 0xEF && (unsigned char)sAll[1] == 0xBB && (unsigned char)sAll[2] == 0xBF )
        sAll.erase( 0, 3 );

    std::string sSection;
    size_t pos = 0;
    while ( pos < sAll.size() )
    {
        size_t eol = sAll.find( '\n', pos );
        if ( eol == std::string::npos ) eol = sAll.size();
        std::string sLine = Trim( sAll.substr( pos, eol - pos ) );
        pos = eol + 1;
        if ( sLine.empty() || sLine[0] == ';' || sLine[0] == '#' ) continue;
        if ( sLine[0] == '[' )
        {
            size_t e = sLine.find( ']' );
            sSection = Trim( sLine.substr( 1, e == std::string::npos ? std::string::npos : e - 1 ) );
            continue;
        }
        size_t eq = sLine.find( '=' );
        if ( eq == std::string::npos ) continue;
        m_mData[sSection][Trim( sLine.substr( 0, eq ) )] = Trim( sLine.substr( eq + 1 ) );
    }
    return true;
}

std::string IniFile::Text() const
{
    std::string s;
    for ( const auto &sec : m_mData )
    {
        s += "[" + sec.first + "]\r\n";
        for ( const auto &kv : sec.second ) s += kv.first + "=" + kv.second + "\r\n";
        s += "\r\n";
    }
    return s;
}

bool IniFile::Save( const std::wstring &sPath ) const
{
    std::wstring sTemp = sPath + L".tmp";
    FILE *f = _wfopen( sTemp.c_str(), L"wb" );
    if ( !f ) return false;
    std::string s = Text();
    bool bOk = fwrite( s.data(), 1, s.size(), f ) == s.size();
    bOk = fclose( f ) == 0 && bOk;
    // Atomic replace so a crash mid-write never leaves a truncated config.
    return bOk && MoveFileExW( sTemp.c_str(), sPath.c_str(), MOVEFILE_REPLACE_EXISTING );
}

bool IniFile::Has( const std::string &sSection, const std::string &sKey ) const
{
    auto it = m_mData.find( sSection );
    return it != m_mData.end() && it->second.count( sKey );
}

std::string IniFile::GetString( const std::string &sSection, const std::string &sKey, const std::string &sDefault ) const
{
    auto it = m_mData.find( sSection );
    if ( it == m_mData.end() ) return sDefault;
    auto kv = it->second.find( sKey );
    return kv == it->second.end() ? sDefault : kv->second;
}

int IniFile::GetInt( const std::string &sSection, const std::string &sKey, int iDefault ) const
{
    if ( !Has( sSection, sKey ) ) return iDefault;
    return (int)strtol( GetString( sSection, sKey ).c_str(), nullptr, 0 );
}

double IniFile::GetDouble( const std::string &sSection, const std::string &sKey, double dDefault ) const
{
    if ( !Has( sSection, sKey ) ) return dDefault;
    return strtod( GetString( sSection, sKey ).c_str(), nullptr );
}

void IniFile::Set( const std::string &sSection, const std::string &sKey, const std::string &sValue )
{
    m_mData[sSection][sKey] = sValue;
}

void IniFile::SetInt( const std::string &sSection, const std::string &sKey, long long iValue )
{
    Set( sSection, sKey, std::to_string( iValue ) );
}

void IniFile::SetDouble( const std::string &sSection, const std::string &sKey, double dValue )
{
    char buf[64];
    snprintf( buf, sizeof( buf ), "%.17g", dValue );
    Set( sSection, sKey, buf );
}
