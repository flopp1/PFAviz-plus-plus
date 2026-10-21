// Track settings dialog (DESIGN.md 10.2), shown by "Practice Using Custom Settings...": the
// original columns minus "Scored", as a virtual list so songs with tens of thousands of
// (track, channel) rows open instantly.
#pragma once

#include <windows.h>
#include <string>
#include <vector>

struct Song;

// Returns false when cancelled. The vectors are per (track, channel) with notes, in track order.
bool GetCustomSettings( HWND hWndOwner, const Song &song, const std::wstring &sFile,
                        std::vector< bool > &vMuted, std::vector< bool > &vHidden, std::vector< unsigned > &vColors );
