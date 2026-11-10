// The loading window's contents, drawn: a heading, figures in tiles, and sections of rows (a label,
// a bar, a value and a note). It shows the progress while a song loads and the statistics after.
#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace LoadView
{
    struct Tile { std::wstring sValue, sLabel; };
    struct Row
    {
        std::wstring sLabel, sValue, sNote;
        double dBar = -1.0;         // 0-1: a bar this full (-1: none)
        int iIndent = 0;            // a part of the row above it: smaller and dimmer
        bool bActive = false;       // highlighted (the stage running)
    };
    struct Section { std::wstring sTitle; std::vector< Row > vRows; int iColumn = 0; };   // iColumn 1: the second column, when wide enough
    struct Model
    {
        std::wstring sTitle, sSub;  // the file's name, and its folder
        std::wstring sStatus;       // a line under the heading, in the accent colour
        std::wstring sAlert;        // a failure, in a banner
        std::vector< Tile > vTiles;
        std::vector< Section > vSections;
    };

    // The window class (registered once): a child window, with WS_VSCROLL and WS_HSCROLL to scroll a report
    // taller than it, or one narrower than the least width the content keeps (it is never cut short)
    const wchar_t *ClassName( HINSTANCE hInstance );
    void Set( HWND hView, Model &&m );      // shown at once (the scroll kept)
    int ContentHeight( HWND hView );        // all of it, in pixels: the window sized to it
}
