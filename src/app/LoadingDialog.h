// Asynchronous song loading with the progress / statistics popup (DESIGN.md 10.4).
#pragma once

#include <windows.h>
#include <string>

#define WM_APP_LOADDONE ( WM_APP + 1 )
#define WM_APP_RENDERVIDEO ( WM_APP + 2 )     // PFAVIZ_RENDER: lParam is the output file
#define WM_APP_RENDERSONG ( WM_APP + 3 )      // File > Render Song: the song is open (without playback); show the render dialog

// Starts loading a file on a background thread and shows the loading popup. Returns false if a load
// is already running. When the load ends, WM_APP_LOADDONE is posted to the main window with the job
// in lParam; the main window then calls FinishLoad.
// bRenderOnly (File > Render Song): opened without playback (no synth), then the render dialog opens
bool StartLoad( const std::wstring &sFile, bool bRenderOnly = false );
void FinishLoad( LPARAM lParam );
bool IsLoading();
// The full path of the song last opened (empty before the first)
const std::wstring &GetOpenedFile();
HWND GetLoadingDialog();
