// Port of the original TracksProc as an LVS_OWNERDATA list: rows are generated on demand, the
// checkboxes and colours are custom-drawn exactly as before.
#include "app/TrackSettings.h"

#include <commctrl.h>
#include <commdlg.h>
#include <cstdio>

#include "app/MainWindow.h"
#include "core/Config.h"
#include "core/Util.h"
#include "midi/Notes.h"
#include "resource.h"
#include "song/Song.h"

namespace
{
    struct TrackDlg
    {
        const Song *pSong;
        std::wstring sFile;
        std::vector< std::pair< int, int > > vRows;     // (track, channel) with notes
        std::vector< bool > vMuted, vHidden;
        std::vector< unsigned > vColors;
        wchar_t aText[256];
    };

    void DefaultColors( TrackDlg &d )
    {
        const VisualSettings &cVisual = Config::GetConfig().GetVisualSettings();
        size_t iMax = sizeof( cVisual.colors ) / sizeof( cVisual.colors[0] );
        for ( size_t i = 0; i < d.vColors.size(); i++ )
            d.vColors[i] = i < iMax ? cVisual.colors[i] : Util::RandColor();
    }

    INT_PTR CALLBACK TracksProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        TrackDlg *d = ( TrackDlg * )GetWindowLongPtrW( hWnd, GWLP_USERDATA );
        switch ( msg )
        {
            case WM_INITDIALOG:
            {
                d = ( TrackDlg * )lParam;
                SetWindowLongPtrW( hWnd, GWLP_USERDATA, ( LONG_PTR )d );
                HWND hWndTracks = GetDlgItem( hWnd, IDC_TRACKS );
                SendMessageW( hWndTracks, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_DOUBLEBUFFER );
                const RefMidiInfo &mInfo = d->pSong->info;

                // Static text
                size_t iSlash = d->sFile.find_last_of( L'\\' );
                SetDlgItemTextW( hWnd, IDC_FILE, iSlash == std::wstring::npos ? d->sFile.c_str() : d->sFile.c_str() + iSlash + 1 );
                SetDlgItemTextW( hWnd, IDC_FOLDER, iSlash == std::wstring::npos ? L"" : d->sFile.substr( 0, iSlash ).c_str() );
                wchar_t buf[64];
                Util::CommaPrintf( buf, (long long)mInfo.llNoteCount );
                SetDlgItemTextW( hWnd, IDC_NOTES, buf );
                swprintf( buf, 64, L"%lld:%02.0lf", mInfo.llTotalMicroSecs / 60000000, ( mInfo.llTotalMicroSecs % 60000000 ) / 1000000.0 );
                SetDlgItemTextW( hWnd, IDC_LENGTH, buf );

                // Rows and state
                for ( size_t t = 0; t < d->pSong->vTracks.size(); t++ )
                    for ( int c = 0; c < 16; c++ )
                        if ( d->pSong->vTracks[t].aNoteCount[c] > 0 ) d->vRows.push_back( { (int)t, c } );
                d->vMuted.assign( d->vRows.size(), false );
                d->vHidden.assign( d->vRows.size(), false );
                d->vColors.resize( d->vRows.size() );
                DefaultColors( *d );

                // Columns
                RECT rcTracks;
                GetClientRect( hWndTracks, &rcTracks );
                const int aFmt[6] = { LVCFMT_LEFT, LVCFMT_LEFT, LVCFMT_RIGHT, LVCFMT_CENTER, LVCFMT_CENTER, LVCFMT_CENTER };
                int aCx[6] = { 27, (int)rcTracks.right - 27 - 55 - 50 - 50 - 50, 55, 50, 50, 50 };
                const wchar_t *aText[6] = { L"Trk", L"Instrument", L"Notes", L"Muted", L"Hidden", L"Color" };
                if ( d->vRows.size() >= 1000 ) aCx[0] = 45; // wider row numbers for big songs
                aCx[1] = (int)rcTracks.right - aCx[0] - 55 - 50 - 50 - 50;
                LVCOLUMNW lvc = {};
                lvc.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT;
                for ( int i = 0; i < 6; i++ )
                {
                    lvc.fmt = aFmt[i];
                    lvc.cx = aCx[i];
                    lvc.pszText = const_cast< wchar_t * >( aText[i] );
                    SendMessageW( hWndTracks, LVM_INSERTCOLUMNW, i, ( LPARAM )&lvc );
                }
                SendMessageW( hWndTracks, LVM_SETITEMCOUNT, d->vRows.size(), 0 );
                if ( GetWindowLongPtrW( hWndTracks, GWL_STYLE ) & WS_VSCROLL )
                    SendMessageW( hWndTracks, LVM_SETCOLUMNWIDTH, 1, aCx[1] - 17 );
                return TRUE;
            }
            case WM_NOTIFY:
            {
                LPNMHDR lpnmhdr = ( LPNMHDR )lParam;
                if ( !d || lpnmhdr->idFrom != IDC_TRACKS ) break;
                switch ( lpnmhdr->code )
                {
                    // Rows are generated on demand
                    case LVN_GETDISPINFOW:
                    {
                        NMLVDISPINFOW *pdi = ( NMLVDISPINFOW * )lParam;
                        int iItem = pdi->item.iItem;
                        if ( !( pdi->item.mask & LVIF_TEXT ) || iItem < 0 || iItem >= (int)d->vRows.size() ) return TRUE;
                        int t = d->vRows[iItem].first, c = d->vRows[iItem].second;
                        const RefTrackInfo &ti = d->pSong->vTracks[t];
                        switch ( pdi->item.iSubItem )
                        {
                            case 0: swprintf( d->aText, 256, L"%d", iItem + 1 ); break;
                            case 1: swprintf( d->aText, 256, L"%ls", c == Notes::Drums ? L"Drums" : Notes::Instruments[ti.aProgram[c] & 0x7F] ); break;
                            case 2: swprintf( d->aText, 256, L"%lld", (long long)ti.aNoteCount[c] ); break;
                            default: d->aText[0] = 0; break;
                        }
                        pdi->item.pszText = d->aText;
                        return TRUE;
                    }
                    // Prevents item selection
                    case LVN_ITEMCHANGING:
                    {
                        LPNMLISTVIEW pnmv = ( LPNMLISTVIEW )lParam;
                        if ( ( pnmv->uChanged & LVIF_STATE ) && ( pnmv->uNewState & LVIS_SELECTED ) != ( pnmv->uOldState & LVIS_SELECTED ) )
                        {
                            SetWindowLongPtrW( hWnd, DWLP_MSGRESULT, TRUE );
                            return TRUE;
                        }
                        break;
                    }
                    // Header click: turn all on or all off, or reset colours
                    case LVN_COLUMNCLICK:
                    {
                        LPNMLISTVIEW lpnmlv = ( LPNMLISTVIEW )lParam;
                        auto ToggleAll = []( std::vector< bool > &v ) {
                            bool bAll = true;
                            for ( size_t i = 0; i < v.size(); i++ ) bAll = bAll && v[i];
                            for ( size_t i = 0; i < v.size(); i++ ) v[i] = !bAll;
                        };
                        if ( lpnmlv->iSubItem == 3 ) ToggleAll( d->vMuted );
                        else if ( lpnmlv->iSubItem == 4 ) ToggleAll( d->vHidden );
                        else if ( lpnmlv->iSubItem == 5 ) DefaultColors( *d );
                        InvalidateRect( lpnmlv->hdr.hwndFrom, nullptr, FALSE );
                        return TRUE;
                    }
                    // Toggle checkboxes or choose a colour
                    case NM_CLICK:
                    case NM_DBLCLK:
                    {
                        LPNMITEMACTIVATE lpnmia = ( LPNMITEMACTIVATE )lpnmhdr;
                        LVHITTESTINFO lvhti = {};
                        lvhti.pt = lpnmia->ptAction;
                        SendMessageW( lpnmia->hdr.hwndFrom, LVM_SUBITEMHITTEST, 0, ( LPARAM )&lvhti );
                        if ( lvhti.iItem < 0 || lvhti.iItem >= (int)d->vRows.size() ) return FALSE;
                        RECT rcItem = { LVIR_BOUNDS };
                        SendMessageW( lpnmia->hdr.hwndFrom, LVM_GETITEMRECT, lvhti.iItem, ( LPARAM )&rcItem );
                        switch ( lvhti.iSubItem )
                        {
                            case 3: d->vMuted[lvhti.iItem] = !d->vMuted[lvhti.iItem]; break;
                            case 4: d->vHidden[lvhti.iItem] = !d->vHidden[lvhti.iItem]; break;
                            case 5:
                            {
                                static COLORREF acrCustClr[16] = { 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF,
                                                                   0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF, 0x00FFFFFF };
                                CHOOSECOLORW cc = {};
                                cc.lStructSize = sizeof( cc );
                                cc.hwndOwner = hWnd;
                                cc.lpCustColors = acrCustClr;
                                cc.rgbResult = d->vColors[lvhti.iItem];
                                cc.Flags = CC_FULLOPEN | CC_RGBINIT;
                                if ( !ChooseColorW( &cc ) ) return TRUE;
                                d->vColors[lvhti.iItem] = cc.rgbResult;
                                break;
                            }
                            default: return FALSE;
                        }
                        InvalidateRect( lpnmia->hdr.hwndFrom, &rcItem, FALSE );
                        return TRUE;
                    }
                    // Checkboxes and colours
                    case NM_CUSTOMDRAW:
                    {
                        LPNMCUSTOMDRAW lpnmcd = ( LPNMCUSTOMDRAW )lParam;
                        LPNMLVCUSTOMDRAW lpnmlvcd = ( LPNMLVCUSTOMDRAW )lParam;
                        switch ( lpnmcd->dwDrawStage )
                        {
                            case CDDS_PREPAINT: case CDDS_ITEMPREPAINT:
                                SetWindowLongPtrW( hWnd, DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW );
                                return TRUE;
                            case CDDS_SUBITEM | CDDS_ITEMPREPAINT:
                            {
                                size_t iItem = lpnmcd->dwItemSpec;
                                if ( lpnmlvcd->iSubItem >= 3 && lpnmlvcd->iSubItem <= 5 && iItem < d->vRows.size() )
                                {
                                    RECT rcOut;
                                    int iBmpSize = lpnmcd->rc.bottom - lpnmcd->rc.top - 2;
                                    rcOut.left = lpnmcd->rc.left + ( lpnmcd->rc.right - lpnmcd->rc.left - iBmpSize ) / 2;
                                    rcOut.top = lpnmcd->rc.top + ( lpnmcd->rc.bottom - lpnmcd->rc.top - iBmpSize ) / 2;
                                    rcOut.right = rcOut.left + iBmpSize;
                                    rcOut.bottom = rcOut.top + iBmpSize;
                                    if ( lpnmlvcd->iSubItem == 3 )
                                        DrawFrameControl( lpnmcd->hdc, &rcOut, DFC_BUTTON, DFCS_BUTTONCHECK | ( d->vMuted[iItem] ? DFCS_CHECKED : 0 ) );
                                    else if ( lpnmlvcd->iSubItem == 4 )
                                        DrawFrameControl( lpnmcd->hdc, &rcOut, DFC_BUTTON, DFCS_BUTTONCHECK | ( d->vHidden[iItem] ? DFCS_CHECKED : 0 ) );
                                    else
                                    {
                                        SetDCBrushColor( lpnmcd->hdc, lpnmlvcd->clrFace );
                                        FillRect( lpnmcd->hdc, &lpnmcd->rc, ( HBRUSH )GetStockObject( DC_BRUSH ) );
                                        InflateRect( &lpnmcd->rc, -1, -1 );
                                        SetDCBrushColor( lpnmcd->hdc, d->vColors[iItem] );
                                        FillRect( lpnmcd->hdc, &lpnmcd->rc, ( HBRUSH )GetStockObject( DC_BRUSH ) );
                                        DrawEdge( lpnmcd->hdc, &lpnmcd->rc, BDR_SUNKENINNER, BF_RECT );
                                    }
                                    SetWindowLongPtrW( hWnd, DWLP_MSGRESULT, CDRF_SKIPDEFAULT );
                                }
                                else
                                    SetWindowLongPtrW( hWnd, DWLP_MSGRESULT, CDRF_DODEFAULT );
                                return TRUE;
                            }
                        }
                        break;
                    }
                }
                break;
            }
            case WM_COMMAND:
                if ( LOWORD( wParam ) == IDOK || LOWORD( wParam ) == IDCANCEL )
                {
                    EndDialog( hWnd, LOWORD( wParam ) );
                    return TRUE;
                }
                break;
        }
        return FALSE;
    }
}

bool GetCustomSettings( HWND hWndOwner, const Song &song, const std::wstring &sFile,
                        std::vector< bool > &vMuted, std::vector< bool > &vHidden, std::vector< unsigned > &vColors )
{
    TrackDlg d;
    d.pSong = &song;
    d.sFile = sFile;
    INT_PTR iResult = DialogBoxParamW( g_hInstance, MAKEINTRESOURCEW( IDD_TRACKSETTINGS ), hWndOwner, TracksProc, ( LPARAM )&d );
    if ( iResult != IDOK ) return false;
    vMuted = d.vMuted;
    vHidden = d.vHidden;
    vColors = d.vColors;
    return true;
}
