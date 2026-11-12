// Every window with a title bar opens where it can be grabbed: when it is shown, its title bar is moved
// onto a screen if it is not there (wholly on one screen's work area from top to bottom, and at least half
// its width, or 200 pixels, across the screens), the window put wholly on the nearest screen's work area
// when it fits there, else at its top left. Once open, it can be dragged anywhere; a position saved for
// next time is saved as it would open (Fixed). Minimized, maximized and full screen windows (no title
// bar) are left alone.
#pragma once

#include <windows.h>

namespace OnScreen
{
    // Watches the windows of the calling thread from now on (a hook of its own; once per thread)
    void Install();
    // Moves this one now, if it needs it; true when it did
    bool Keep( HWND hWnd );
    // Where a window with title bar of this size at r would be put (r's own top left when it is fine)
    POINT Fixed( const RECT &r );
}
