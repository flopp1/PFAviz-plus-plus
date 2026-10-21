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
VOID SetOnTop( BOOL bOnTop );
VOID SetFullScreen( BOOL bFullScreen );
VOID SetZoomMove( BOOL bZoomMove );
VOID SetMute( BOOL bMute );
VOID SetSpeed( double dSpeed );
VOID SetNSpeed( double dSpeed );
VOID SetVolume( double dVolume );
VOID SetPosition( INT iPosition );
VOID SetMetronome( INT iMetronome );
VOID SetPlayable( BOOL bPlayable );
VOID SetPlayMode( INT ePlayMode );
VOID SetPlayPauseStop( BOOL bPlay, BOOL bPause, BOOL bStop );
BOOL PlayFile( const std::wstring &sFile, bool bCustomSettings = false );
VOID CheckActivity( BOOL bIsActive, POINT *ptNew = nullptr, BOOL bToggleEnable = false );
