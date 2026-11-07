// Minimal UTF-8 INI file: [section] / key=value. Replaces TinyXML.
#pragma once

#include <map>
#include <vector>
#include <set>
#include <string>

class IniFile
{
public:
    bool Load( const std::wstring &sPath );
    bool Save( const std::wstring &sPath ) const;
    std::string Text() const;           // the file's contents, as Save writes them

    bool Has( const std::string &sSection, const std::string &sKey ) const;
    bool HasSection( const std::string &sSection ) const { return m_mData.count( sSection ) != 0; }
    // A section's keys and values, in key order (none: empty)
    std::vector< std::pair< std::string, std::string > > Section( const std::string &sSection ) const;
    std::string GetString( const std::string &sSection, const std::string &sKey, const std::string &sDefault = "" ) const;
    int GetInt( const std::string &sSection, const std::string &sKey, int iDefault ) const;
    double GetDouble( const std::string &sSection, const std::string &sKey, double dDefault ) const;
    bool GetBool( const std::string &sSection, const std::string &sKey, bool bDefault ) const { return GetInt( sSection, sKey, bDefault ) != 0; }

    void Set( const std::string &sSection, const std::string &sKey, const std::string &sValue );
    void SetInt( const std::string &sSection, const std::string &sKey, long long iValue );
    void SetDouble( const std::string &sSection, const std::string &sKey, double dValue );

    // Another file's keys over these
    void Overlay( const IniFile &o );
    // The keys of mine that changed since before (or that this lacks) over these, the rest kept as they
    // are: another program's changes, and keys it saved that mine does not know. A section in
    // whole (a list: its count and items) is replaced all at once when any of it changed.
    void MergeChanges( const IniFile &mine, const IniFile &before, const std::set< std::string > &whole );

private:
    std::map< std::string, std::map< std::string, std::string > > m_mData;
};
