// Preferences property sheet (DESIGN.md 10.3): the original Visual, Audio and Controls tabs, minus
// what needed MIDI input, plus a Performance tab in the Library tab's place.
#pragma once

#include <windows.h>

// Opens the Preferences window, modeless (the main window stays usable), or brings it forward when open
VOID DoPreferences( HWND hWndOwner );
// For the message loop: a message for the Preferences window handled (TRUE: not to be translated or
// dispatched again); the window closed and destroyed once OK, Cancel or its close button ends it
BOOL PreferencesMessage( MSG *pMsg );
