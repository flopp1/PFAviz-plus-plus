// The interface's dark theme (Preferences, Visual: the theme): title bars, menus, the playback bar and every dialog
// drawn dark. Install once at the start: it watches the windows the UI thread makes, so the app's
// dialogs (and their property pages) are darkened as they open. SetTheme switches, redrawing the
// open windows. Windows' own dark mode is used where it has one (title bars, pop-up menus, push
// buttons, edits, combo boxes, scroll bars, list views); the rest is drawn here (the menu bar,
// check boxes, radio buttons, group boxes, tabs, and the colours of text and backgrounds).
#pragma once

#include <windows.h>
#include <commctrl.h>

namespace DarkMode
{
    enum Theme { ThemeLight, ThemeDark, ThemeSystem, ThemeCount };
    void Install();
    void SetTheme( int eTheme );
    bool On();
    unsigned Generation();                  // counts the switches: drawings cached under the old theme are made again
    void OnSettingChange( LPARAM lParam );  // WM_SETTINGCHANGE: ThemeSystem follows Windows' setting

    // The colours (when On)
    COLORREF Back();                        // windows and dialogs
    COLORREF Field();                       // edits, lists, the position slider's channel
    COLORREF Text();
    COLORREF Edge();
    HBRUSH BackBrush();
    HBRUSH FieldBrush();

    // A bitmap's images (each cx wide, cMask their transparent colour), their greys turned over when
    // On: the toolbar's dark icons light
    HIMAGELIST Images( HINSTANCE hInstance, UINT uId, int cx, COLORREF cMask );

    // A window not made from a dialog template (the main window, its playback bar): it and its
    // children themed now, and again by SetTheme
    void Apply( HWND hWnd );
    // A dialog the hook does not see (a property sheet's frame): darkened like the app's own
    void Dialog( HWND hDlg );
    // For a window procedure: WM_CTLCOLOR* answered dark (false when not dark: do as before)
    bool CtlColor( UINT msg, WPARAM wParam, LRESULT &lr, HWND hCtl = nullptr );     // hCtl: the control (disabled: its text dim)
    // For the main window's procedure: its menu bar drawn dark (false: not handled)
    bool MenuBar( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT &lr );
}
