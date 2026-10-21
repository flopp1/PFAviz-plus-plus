// Minimal UTF-8 INI file: [section] / key=value. Replaces TinyXML.
#pragma once

#include <map>
#include <string>

class IniFile
{
public:
    bool Load( const std::wstring &sPath );
    bool Save( const std::wstring &sPath ) const;

    bool Has( const std::string &sSection, const std::string &sKey ) const;
    std::string GetString( const std::string &sSection, const std::string &sKey, const std::string &sDefault = "" ) const;
    int GetInt( const std::string &sSection, const std::string &sKey, int iDefault ) const;
    double GetDouble( const std::string &sSection, const std::string &sKey, double dDefault ) const;
    bool GetBool( const std::string &sSection, const std::string &sKey, bool bDefault ) const { return GetInt( sSection, sKey, bDefault ) != 0; }

    void Set( const std::string &sSection, const std::string &sKey, const std::string &sValue );
    void SetInt( const std::string &sSection, const std::string &sKey, long long iValue );
    void SetDouble( const std::string &sSection, const std::string &sKey, double dValue );

private:
    std::map< std::string, std::map< std::string, std::string > > m_mData;
};
