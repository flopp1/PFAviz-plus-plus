// The loading window's contents, drawn: a heading, figures in tiles, and sections of rows (a label,
// a bar, a value and a note). It shows the progress while a song loads and the statistics after, and
// the video renderer's progress (with its preview, a picture drawn under the heading).
#pragma once

#include <windows.h>
#include <functional>
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
        // Progress: the bars glide to each new value over about the time since the last (so a value
        // updated a few times a second moves smoothly); a bar going back jumps. Off: they are drawn as given
        bool bSmooth = false;
        // A picture under the heading, drawn into the rectangle given (the view's height left over by the
        // rest, at least iImageMinCY); none when not set
        std::function< void( HDC, const RECT & ) > fnImage;
        int iImageMinCY = 0;
    };

    // The window class (registered once): a child window, with WS_VSCROLL and WS_HSCROLL to scroll a report
    // taller than it, or one narrower than the least width the content keeps (it is never cut short)
    const wchar_t *ClassName( HINSTANCE hInstance );
    void Set( HWND hView, Model &&m );      // shown at once (the scroll kept; smooth bars glide from where they are)
    int ContentHeight( HWND hView );        // all of it, in pixels: the window sized to it
}
