#include "app/LoadView.h"

#include <uxtheme.h>
#include <algorithm>

#include "app/DarkMode.h"
#include "core/Util.h"

namespace
{
    const wchar_t *const Class = L"PFAvizLoadView";

    // A bar gliding from one value to the next (Model::bSmooth): from at t0 to to at t0 + dur
    struct Glide { double from = 0, to = 0; long long t0 = 0, dur = 1; std::wstring sKey; };
    double GlideAt( const Glide &g, long long llNow )
    {
        const double t = std::clamp( (double)( llNow - g.t0 ) / (double)g.dur, 0.0, 1.0 );
        return g.from + ( g.to - g.from ) * t;
    }
    const UINT_PTR GlideTimer = 1;
    const UINT GlideMs = 15;                // (about 60 a second while a bar moves)

    struct State
    {
        LoadView::Model m;
        std::vector< Glide > vGlide;        // the rows' bars, in order (smooth models)
        long long llLastSet = 0;
        bool bGliding = false;
        int iImageCY = 0;                   // the picture's height, as laid out
        int iScroll = 0, iScrollX = 0;
        int iContent = 0;           // the content's height, laid out iContentCX wide
        int iContentCX = 0;         // the view's width, or MinContentCX when narrower (then scrolled sideways)
        bool bUpdating = false;     // (a scroll bar shown or hidden resizes the view: not laid out again inside)
        unsigned uThemeGen = ~0u;
    };

    struct Colors { COLORREF back, card, text, dim, accent, accentSoft, track, line, alertBack, alertText; };
    Colors GetColors()
    {
        if ( DarkMode::On() )
            return { RGB( 32, 32, 32 ), RGB( 44, 44, 46 ), RGB( 235, 235, 235 ), RGB( 150, 150, 155 ), RGB( 96, 165, 250 ),
                     RGB( 62, 98, 140 ), RGB( 62, 62, 66 ), RGB( 58, 58, 60 ), RGB( 78, 36, 36 ), RGB( 255, 190, 190 ) };
        return { RGB( 255, 255, 255 ), RGB( 244, 245, 248 ), RGB( 28, 28, 30 ), RGB( 108, 110, 118 ), RGB( 37, 99, 235 ),
                 RGB( 140, 175, 245 ), RGB( 226, 229, 235 ), RGB( 230, 231, 235 ), RGB( 253, 234, 234 ), RGB( 160, 30, 30 ) };
    }

    struct Fonts { HFONT hTitle, hBody, hBold, hSmall, hTile, hHeading; };
    const Fonts &GetFonts()
    {
        static const Fonts f = [] {
            auto F = []( int iPx, int iWeight ) {
                return CreateFontW( -iPx, 0, 0, 0, iWeight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI" );
            };
            return Fonts{ F( 20, FW_SEMIBOLD ), F( 13, FW_NORMAL ), F( 13, FW_SEMIBOLD ), F( 12, FW_NORMAL ), F( 20, FW_SEMIBOLD ), F( 11, FW_BOLD ) };
        }();
        return f;
    }

    // The sizes, in pixels (the program is DPI-unaware: Windows scales it)
    // The least width the content is laid out at: one column, nothing cut short (narrower, it scrolls sideways;
    // two columns give way to one first)
    const int MinContentCX = 540;
    const int Margin = 18, TileCY = 62, TileGap = 10, RowCY = 23, PartCY = 20, SectionGap = 12;

    void Fill( HDC hdc, int x, int y, int cx, int cy, COLORREF c, int iRadius )
    {
        if ( cx <= 0 || cy <= 0 ) return;
        SelectObject( hdc, GetStockObject( DC_BRUSH ) );
        SelectObject( hdc, GetStockObject( DC_PEN ) );
        SetDCBrushColor( hdc, c );
        SetDCPenColor( hdc, c );
        if ( iRadius > 0 ) RoundRect( hdc, x, y, x + cx, y + cy, iRadius, iRadius );
        else { RECT rc = { x, y, x + cx, y + cy }; FillRect( hdc, &rc, (HBRUSH)GetStockObject( DC_BRUSH ) ); }
    }

    void Text( HDC hdc, const std::wstring &s, HFONT hFont, COLORREF c, int x, int y, int cx, int cy, UINT uFormat )
    {
        if ( s.empty() || cx <= 0 ) return;
        SelectObject( hdc, hFont );
        SetTextColor( hdc, c );
        RECT rc = { x, y, x + cx, y + cy };
        DrawTextW( hdc, s.c_str(), (int)s.size(), &rc, uFormat | DT_NOPREFIX );
    }

    const UINT Line = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;

    // Lays the model out from y0 (scrolled), drawing when bDraw; returns its height. pBars: the bars as
    // shown, row by row (null: as the model has them); iImageCY: the picture's height
    int Layout( HDC hdc, const LoadView::Model &m, int cx, int y0, bool bDraw, const std::vector< double > *pBars = nullptr, int iImageCY = 0 )
    {
        const Colors c = GetColors();
        const Fonts &f = GetFonts();
        const int W = cx - Margin * 2;
        int y = y0 + Margin;

        // The heading: the file's name, its folder, and a status line
        if ( bDraw ) Text( hdc, m.sTitle, f.hTitle, c.text, Margin, y, W, 28, Line );
        y += 28;
        if ( !m.sSub.empty() )
        {
            if ( bDraw ) Text( hdc, m.sSub, f.hSmall, c.dim, Margin, y, W, 17, DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS );
            y += 17;
        }
        if ( !m.sStatus.empty() )
        {
            y += 4;
            if ( bDraw ) Text( hdc, m.sStatus, f.hBody, c.accent, Margin, y, W, 18, Line );
            y += 18;
        }
        if ( !m.sAlert.empty() )
        {
            y += 8;
            SelectObject( hdc, f.hBody );
            RECT rc = { 0, 0, W - 24, 0 };
            DrawTextW( hdc, m.sAlert.c_str(), (int)m.sAlert.size(), &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX );
            int cy = rc.bottom + 16;
            if ( bDraw )
            {
                Fill( hdc, Margin, y, W, cy, c.alertBack, 8 );
                Text( hdc, m.sAlert, f.hBody, c.alertText, Margin + 12, y + 8, W - 24, cy - 16, DT_WORDBREAK );
            }
            y += cy;
        }
        y += 14;

        // The picture, as wide as the content and as tall as it was given
        if ( m.fnImage && iImageCY > 0 )
        {
            if ( bDraw )
            {
                RECT rc = { Margin, y, Margin + W, y + iImageCY };
                SaveDC( hdc );
                IntersectClipRect( hdc, rc.left, rc.top, rc.right, rc.bottom );
                m.fnImage( hdc, rc );
                RestoreDC( hdc, -1 );
            }
            y += iImageCY + 14;
        }

        // The figures, in tiles across
        if ( !m.vTiles.empty() )
        {
            int n = (int)m.vTiles.size();
            int tw = ( W - TileGap * ( n - 1 ) ) / n;
            for ( int i = 0; i < n && bDraw; i++ )
            {
                int x = Margin + i * ( tw + TileGap );
                Fill( hdc, x, y, tw, TileCY, c.card, 10 );
                Text( hdc, m.vTiles[i].sValue, f.hTile, c.text, x + 12, y + 8, tw - 20, 28, Line );
                Text( hdc, m.vTiles[i].sLabel, f.hSmall, c.dim, x + 12, y + 37, tw - 20, 17, Line );
            }
            y += TileCY + 18;
        }

        // The sections: a heading over a hairline, then the rows; at x, cw wide
        size_t iRow = 0;
        auto DrawSection = [&]( const LoadView::Section &s, const int X, const int CW, int y ) -> int
        {
            if ( s.vRows.empty() ) return y;
            if ( bDraw )
            {
                Text( hdc, s.sTitle, f.hHeading, c.dim, X, y, CW, 16, Line );
                Fill( hdc, X, y + 19, CW, 1, c.line, 0 );
            }
            y += 25;
            bool bBars = false, bNotes = false;
            for ( const LoadView::Row &r : s.vRows ) { bBars |= r.dBar >= 0.0; bNotes |= !r.sNote.empty(); }
            // The columns: the label, then (with bars) the bar, the value right-aligned and the note;
            // without, the value straight after the label
            const int iLabelCX = std::clamp( CW * 34 / 100, 120, 220 );
            const int iNoteCX = bNotes ? std::clamp( CW * 20 / 100, 90, 140 ) : 0;
            const int iValueCX = bBars ? 76 : CW - iLabelCX;
            const int iBarX = X + iLabelCX, iBarCX = CW - iLabelCX - iValueCX - iNoteCX - 12;
            for ( const LoadView::Row &r : s.vRows )
            {
                const bool bPart = r.iIndent > 0;
                const int cy = bPart ? PartCY : RowCY;
                const double dBar = pBars && iRow < pBars->size() && r.dBar >= 0.0 ? ( *pBars )[iRow] : r.dBar;
                iRow++;
                if ( bDraw )
                {
                    const int iIndent = r.iIndent * 14;
                    HFONT hLabel = r.bActive ? f.hBold : bPart ? f.hSmall : f.hBody;
                    COLORREF cLabel = r.bActive ? c.accent : bPart ? c.dim : c.text;
                    Text( hdc, r.sLabel, hLabel, cLabel, X + iIndent, y, iLabelCX - iIndent - 8, cy, Line );
                    if ( bBars )
                    {
                        if ( dBar >= 0.0 && iBarCX > 20 )
                        {
                            const int h = bPart ? 4 : 6, by = y + ( cy - h ) / 2;
                            Fill( hdc, iBarX, by, iBarCX, h, c.track, h );
                            int iFill = (int)( std::clamp( dBar, 0.0, 1.0 ) * iBarCX + 0.5 );
                            if ( iFill > 0 ) Fill( hdc, iBarX, by, std::max( iFill, h ), h, bPart ? c.accentSoft : c.accent, h );
                        }
                        Text( hdc, r.sValue, bPart ? f.hSmall : f.hBody, bPart ? c.dim : c.text, iBarX + std::max( iBarCX, 0 ) + 8, y, iValueCX, cy,
                              Line | DT_RIGHT );
                    }
                    else
                        Text( hdc, r.sValue, bPart ? f.hSmall : f.hBody, bPart ? c.dim : c.text, X + iLabelCX, y, iValueCX - iNoteCX, cy, Line );
                    if ( bNotes ) Text( hdc, r.sNote, f.hSmall, c.dim, X + CW - iNoteCX, y, iNoteCX, cy, Line | DT_RIGHT );
                }
                y += cy;
            }
            return y + SectionGap;
        }
        ;
        // Two columns when there is room (the sections marked for the second beside the rest), else one
        const int ColGap = 28;
        bool bTwo = W >= 2 * 380 + ColGap && std::any_of( m.vSections.begin(), m.vSections.end(), []( const LoadView::Section &s ) { return s.iColumn == 1; } );
        if ( bTwo )
        {
            const int CW = ( W - ColGap ) / 2;
            int ay[2] = { y, y };
            for ( const LoadView::Section &s : m.vSections )
                ay[s.iColumn ? 1 : 0] = DrawSection( s, Margin + ( s.iColumn ? CW + ColGap : 0 ), CW, ay[s.iColumn ? 1 : 0] );
            y = std::max( ay[0], ay[1] );
        }
        else
            for ( const LoadView::Section &s : m.vSections ) y = DrawSection( s, Margin, W, y );
        return y - y0 + Margin - SectionGap;
    }

    void UpdateScroll( HWND hWnd, State *p )
    {
        if ( p->bUpdating ) return;
        p->bUpdating = true;
        // (each scroll bar shown or hidden changes the room for the other: laid out again until it settles)
        for ( int i = 0; i < 3; i++ )
        {
            RECT rc;
            GetClientRect( hWnd, &rc );
            p->iContentCX = std::max< int >( rc.right, MinContentCX );
            HDC hdc = GetDC( hWnd );
            p->iContent = Layout( hdc, p->m, p->iContentCX, 0, false );
            // (the picture takes the height the rest leaves, at least its least)
            p->iImageCY = 0;
            if ( p->m.fnImage )
            {
                p->iImageCY = std::max( p->m.iImageMinCY, (int)rc.bottom - p->iContent - 14 );
                p->iContent += p->iImageCY + 14;
            }
            ReleaseDC( hWnd, hdc );
            p->iScroll = std::clamp( p->iScroll, 0, std::max( 0, p->iContent - (int)rc.bottom ) );
            p->iScrollX = std::clamp( p->iScrollX, 0, std::max( 0, p->iContentCX - (int)rc.right ) );
            SCROLLINFO si = { sizeof( si ), SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max( 0, p->iContent - 1 ), (UINT)rc.bottom, p->iScroll };
            SetScrollInfo( hWnd, SB_VERT, &si, TRUE );
            SCROLLINFO sx = { sizeof( sx ), SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max( 0, p->iContentCX - 1 ), (UINT)rc.right, p->iScrollX };
            SetScrollInfo( hWnd, SB_HORZ, &sx, TRUE );
            RECT rcAfter;
            GetClientRect( hWnd, &rcAfter );
            if ( rcAfter.right == rc.right && rcAfter.bottom == rc.bottom ) break;
        }
        p->bUpdating = false;
    }

    void ScrollTo( HWND hWnd, State *p, int iBar, int iPos )
    {
        RECT rc;
        GetClientRect( hWnd, &rc );
        const bool bVert = iBar == SB_VERT;
        int &iScroll = bVert ? p->iScroll : p->iScrollX;
        iPos = std::clamp( iPos, 0, std::max( 0, bVert ? p->iContent - (int)rc.bottom : p->iContentCX - (int)rc.right ) );
        if ( iPos == iScroll ) return;
        iScroll = iPos;
        SetScrollPos( hWnd, iBar, iPos, TRUE );
        InvalidateRect( hWnd, nullptr, FALSE );
    }

    // A scroll bar's message: where it moves to
    int ScrollPos( HWND hWnd, State *p, int iBar, WPARAM wParam )
    {
        RECT rc;
        GetClientRect( hWnd, &rc );
        const bool bVert = iBar == SB_VERT;
        const int iPage = bVert ? rc.bottom : rc.right, iLine = bVert ? RowCY : 40;
        SCROLLINFO si = { sizeof( si ), SIF_TRACKPOS };
        GetScrollInfo( hWnd, iBar, &si );
        int iPos = bVert ? p->iScroll : p->iScrollX;
        switch ( LOWORD( wParam ) )
        {
            case SB_LINEUP: iPos -= iLine; break;
            case SB_LINEDOWN: iPos += iLine; break;
            case SB_PAGEUP: iPos -= iPage - iLine; break;
            case SB_PAGEDOWN: iPos += iPage - iLine; break;
            case SB_THUMBTRACK: case SB_THUMBPOSITION: iPos = si.nTrackPos; break;
            case SB_TOP: iPos = 0; break;
            case SB_BOTTOM: iPos = bVert ? p->iContent : p->iContentCX; break;
        }
        return iPos;
    }

    LRESULT CALLBACK ViewProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
    {
        State *p = reinterpret_cast< State * >( GetWindowLongPtrW( hWnd, GWLP_USERDATA ) );
        switch ( msg )
        {
            case WM_NCCREATE:
                SetWindowLongPtrW( hWnd, GWLP_USERDATA, ( LONG_PTR )new State() );
                break;
            case WM_NCDESTROY:
                delete p;
                SetWindowLongPtrW( hWnd, GWLP_USERDATA, 0 );
                break;
            case WM_SIZE:
                if ( p ) { UpdateScroll( hWnd, p ); InvalidateRect( hWnd, nullptr, FALSE ); }
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT:
            {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint( hWnd, &ps );
                if ( p && p->uThemeGen != DarkMode::Generation() )
                {
                    p->uThemeGen = DarkMode::Generation();
                    SetWindowTheme( hWnd, DarkMode::On() ? L"DarkMode_Explorer" : nullptr, nullptr );     // (its scroll bar)
                }
                RECT rc;
                GetClientRect( hWnd, &rc );
                HDC hMem = CreateCompatibleDC( hdc );
                HBITMAP hBmp = CreateCompatibleBitmap( hdc, std::max( 1, (int)rc.right ), std::max( 1, (int)rc.bottom ) );
                HGDIOBJ hOld = SelectObject( hMem, hBmp );
                SetBkMode( hMem, TRANSPARENT );
                Fill( hMem, 0, 0, rc.right, rc.bottom, GetColors().back, 0 );
                if ( p )
                {
                    std::vector< double > vBars;
                    bool bMoving = false;
                    if ( p->m.bSmooth )
                    {
                        const long long llNow = Timer::NowMicroSecs();
                        for ( const Glide &g : p->vGlide )
                        {
                            vBars.push_back( GlideAt( g, llNow ) );
                            bMoving |= llNow < g.t0 + g.dur && g.from != g.to;
                        }
                    }
                    if ( !bMoving && p->bGliding ) { KillTimer( hWnd, GlideTimer ); p->bGliding = false; }
                    SetViewportOrgEx( hMem, -p->iScrollX, 0, nullptr );
                    Layout( hMem, p->m, std::max< int >( p->iContentCX, rc.right ), -p->iScroll, true, p->m.bSmooth ? &vBars : nullptr, p->iImageCY );
                    SetViewportOrgEx( hMem, 0, 0, nullptr );
                }
                BitBlt( hdc, 0, 0, rc.right, rc.bottom, hMem, 0, 0, SRCCOPY );
                SelectObject( hMem, hOld );
                DeleteObject( hBmp );
                DeleteDC( hMem );
                EndPaint( hWnd, &ps );
                return 0;
            }
            case WM_TIMER:
                if ( wParam == GlideTimer ) { InvalidateRect( hWnd, nullptr, FALSE ); return 0; }
                break;
            case WM_VSCROLL:
            case WM_HSCROLL:
                if ( !p ) break;
                ScrollTo( hWnd, p, msg == WM_VSCROLL ? SB_VERT : SB_HORZ, ScrollPos( hWnd, p, msg == WM_VSCROLL ? SB_VERT : SB_HORZ, wParam ) );
                return 0;
            case WM_MOUSEWHEEL:     // (with Shift: sideways)
                if ( p && ( GET_KEYSTATE_WPARAM( wParam ) & MK_SHIFT ) )
                    ScrollTo( hWnd, p, SB_HORZ, p->iScrollX - GET_WHEEL_DELTA_WPARAM( wParam ) * 120 / WHEEL_DELTA );
                else if ( p ) ScrollTo( hWnd, p, SB_VERT, p->iScroll - GET_WHEEL_DELTA_WPARAM( wParam ) * RowCY * 3 / WHEEL_DELTA );
                return 0;
            case WM_MOUSEHWHEEL:
                if ( p ) ScrollTo( hWnd, p, SB_HORZ, p->iScrollX + GET_WHEEL_DELTA_WPARAM( wParam ) * 120 / WHEEL_DELTA );
                return 0;
        }
        return DefWindowProcW( hWnd, msg, wParam, lParam );
    }
}

const wchar_t *LoadView::ClassName( HINSTANCE hInstance )
{
    static bool bRegistered = false;
    if ( !bRegistered )
    {
        WNDCLASSEXW wc = { sizeof( wc ) };
        wc.lpfnWndProc = ViewProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursorW( nullptr, IDC_ARROW );
        wc.lpszClassName = Class;
        bRegistered = RegisterClassExW( &wc ) != 0;
    }
    return Class;
}

void LoadView::Set( HWND hView, Model &&m )
{
    State *p = reinterpret_cast< State * >( GetWindowLongPtrW( hView, GWLP_USERDATA ) );
    if ( !p ) return;
    // The bars glide from where they are shown to the new values, over the time since the last values
    // came (so steady progress moves at an even pace, a step behind); one going back, or a new row, jumps
    const long long llNow = Timer::NowMicroSecs();
    std::vector< Glide > vGlide;
    if ( m.bSmooth )
    {
        const long long llDur = std::clamp< long long >( llNow - p->llLastSet, 30000, 600000 );
        size_t i = 0;
        bool bMoving = false;
        for ( const Section &s : m.vSections )
            for ( const Row &r : s.vRows )
            {
                Glide g;
                g.sKey = s.sTitle + L"\x1f" + r.sLabel;
                g.to = std::clamp( r.dBar, 0.0, 1.0 );
                g.t0 = llNow;
                g.dur = llDur;
                g.from = g.to;
                if ( p->m.bSmooth && i < p->vGlide.size() && p->vGlide[i].sKey == g.sKey )
                {
                    const double dNow = GlideAt( p->vGlide[i], llNow );
                    if ( g.to >= dNow ) g.from = dNow;
                }
                bMoving |= g.from != g.to;
                vGlide.push_back( g );
                i++;
            }
        if ( bMoving && !p->bGliding ) p->bGliding = SetTimer( hView, GlideTimer, GlideMs, nullptr ) != 0;
    }
    p->vGlide = std::move( vGlide );
    p->llLastSet = llNow;
    p->m = std::move( m );
    UpdateScroll( hView, p );
    InvalidateRect( hView, nullptr, FALSE );
}

int LoadView::ContentHeight( HWND hView )
{
    State *p = reinterpret_cast< State * >( GetWindowLongPtrW( hView, GWLP_USERDATA ) );
    return p ? p->iContent : 0;
}
