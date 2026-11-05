// Main window, graphics child window, rebar toolbar and position slider.
// Port of the original MainProcs.cpp without the library panel, note labels, learn and play modes.
#pragma once

#include <windows.h>
#include <commctrl.h>
#include <string>

#include "core/Util.h"

extern HINSTANCE g_hInstance;
extern HWND g_hWnd;
extern HWND g_hWndBar;
extern HWND g_hWndGfx;
extern SpscQueue< MSG > g_MsgQueue; // UI thread -> game thread

LRESULT WINAPI WndProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );
LRESULT WINAPI GfxProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );
LRESULT WINAPI PosnProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );
HWND CreateRebar( HWND hWndOwner );
HMENU GetMainMenu();
VOID SizeWindows( int iMainWidth, int iMainHeight );

VOID HandOffMsg( UINT msg, WPARAM wParam, LPARAM lParam );
VOID ShowControls( BOOL bShow );
VOID ShowKeyboard( BOOL bShow );
VOID ShowGraphs( BOOL bShow );      // View > Graphs checked or not
VOID SetOnTop( BOOL bOnTop );
VOID SetInterfaceTheme( INT eTheme );     // DarkMode::Theme
VOID SetFullScreen( BOOL bFullScreen );
// The view's size (the graphics window: notes and keyboard, without the title, menu and controls
// bars): its smallest (the window's MINWIDTH x MINHEIGHT) and largest (its monitor's), its size now
// (the restored size when maximized), and setting it: clamped to those, the window moved to lie
// wholly on its monitor (in the work area when it fits there)
SIZE MainWindowMinSize();
SIZE MainWindowMaxSize();
SIZE MainWindowSize();
VOID SetMainWindowSize( int iWidth, int iHeight );
VOID SetZoomMove( BOOL bZoomMove );
VOID SetMute( BOOL bMute );
VOID SetSpeed( double dSpeed );
VOID SetNSpeed( double dSpeed );
// The toolbar's exact speed boxes (the main loop leaves the accelerators to them while one has the focus)
BOOL IsSpeedEdit( HWND hWnd );
// Shows the toolbar with or without the exact speed boxes (without: the original's layout)
VOID ShowSpeedBoxes( BOOL bShow );
VOID SetVolume( double dVolume );
VOID SetPosition( INT iPosition );
VOID SetMetronome( INT iMetronome );
VOID SetPlayable( BOOL bPlayable );
VOID SetPlayMode( INT ePlayMode );
VOID SetPlayPauseStop( BOOL bPlay, BOOL bPause, BOOL bStop );
BOOL PlayFile( const std::wstring &sFile );
VOID CheckActivity( BOOL bIsActive, POINT *ptNew = nullptr, BOOL bToggleEnable = false );
