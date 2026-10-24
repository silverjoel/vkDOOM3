/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company. 
Copyright (C) 2016-2017 Dustin Land

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").  

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#pragma hdrstop
#include "../../framework/precompiled.h"
#include "win_local.h"
#include "../../sound/ClassicAudioCapture.h"
#include <Windowsx.h>

LRESULT CALLBACK MainWndProc( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam );

static bool s_alttab_disabled;

idCVar win_allowAltTab( "win_allowAltTab", "0", CVAR_SYSTEM | CVAR_BOOL, "allow Alt-Tab when fullscreen" );
idCVar win_timerUpdate( "win_timerUpdate", "0", CVAR_SYSTEM | CVAR_BOOL, "allows the game to be updated while dragging the window" );

extern idCVar r_windowX;
extern idCVar r_windowY;
extern idCVar r_windowWidth;
extern idCVar r_windowHeight;

// Session history, independent of the HWND and of unconfirmed menu previews.
static int s_lastFullscreen;
static idStr s_lastFullscreenDevice;
static bool s_fullscreenPreview;
static bool s_fullscreenRestart;

bool R_GetModeListForDisplay( const int displayNum, idList<vidMode_t> & modeList );

static bool WIN_GetFullscreenDevice( int fullscreen, DISPLAY_DEVICE & device ) {
	device = DISPLAY_DEVICE();
	device.cb = sizeof( device );
	if ( fullscreen <= 0 || !EnumDisplayDevices( NULL, fullscreen - 1, &device, 0 ) ||
		!( device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP ) ) {
		return false;
	}
	DISPLAY_DEVICE monitor = {};
	monitor.cb = sizeof( monitor );
	return EnumDisplayDevices( device.DeviceName, 0, &monitor, 0 ) != FALSE;
}

// Called before the menu overwrites r_fullscreen, and before an outward toggle.
void WIN_RememberFullscreen() {
	const int fullscreen = cvarSystem->GetCVarInteger( "r_fullscreen" );
	if ( s_fullscreenPreview || !renderSystem->IsInitialized() ||
		fullscreen != win32.isFullscreen || fullscreen == 0 ) {
		return;
	}
	DISPLAY_DEVICE device;
	if ( fullscreen == -1 || WIN_GetFullscreenDevice( fullscreen, device ) ) {
		s_lastFullscreen = fullscreen;
		s_lastFullscreenDevice = fullscreen == -1 ? "" : device.DeviceName;
	}
}

void WIN_BeginFullscreenPreview() {
	WIN_RememberFullscreen();
	s_fullscreenPreview = true;
}

void WIN_EndFullscreenPreview( bool accepted ) {
	s_fullscreenPreview = false;
	if ( accepted ) {
		WIN_RememberFullscreen();
	}
}

static bool WIN_CanRestoreFullscreen( int fullscreen ) {
	if ( fullscreen == -1 ) {
		return true;
	}
	DISPLAY_DEVICE device;
	if ( !WIN_GetFullscreenDevice( fullscreen, device ) ) {
		return false;
	}
	idList<vidMode_t> modes;
	if ( !R_GetModeListForDisplay( fullscreen - 1, modes ) || modes.Num() == 0 ) {
		return false;
	}
	DEVMODE mode = {};
	mode.dmSize = sizeof( mode );
	const int vidMode = cvarSystem->GetCVarInteger( "r_vidMode" );
	if ( vidMode < 0 ) {
		const int width = cvarSystem->GetCVarInteger( "r_customWidth" );
		const int height = cvarSystem->GetCVarInteger( "r_customHeight" );
		const int hz = cvarSystem->GetCVarInteger( "r_displayRefresh" );
		if ( width <= 0 || height <= 0 || hz < 0 ) {
			return false;
		}
		mode.dmPelsWidth = width;
		mode.dmPelsHeight = height;
		mode.dmDisplayFrequency = hz;
	} else {
		if ( vidMode >= modes.Num() ) {
			return false;
		}
		mode.dmPelsWidth = modes[vidMode].width;
		mode.dmPelsHeight = modes[vidMode].height;
		mode.dmDisplayFrequency = modes[vidMode].displayHz;
	}
	mode.dmBitsPerPel = 32;
	mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
	if ( mode.dmDisplayFrequency != 0 ) {
		mode.dmFields |= DM_DISPLAYFREQUENCY;
	}
	return ChangeDisplaySettingsEx( device.DeviceName, &mode, NULL, CDS_TEST, NULL ) == DISP_CHANGE_SUCCESSFUL;
}

static int WIN_FullscreenTarget() {
	if ( s_lastFullscreen == -1 ) {
		return -1;
	}
	// Keep the display identity if Windows has renumbered the adapters.
	MONITORINFOEX nearest = {};
	nearest.cbSize = sizeof( nearest );
	GetMonitorInfo( MonitorFromWindow( win32.hWnd, MONITOR_DEFAULTTONEAREST ), &nearest );
	int fallback = 0;
	for ( int index = 0; ; ++index ) {
		DISPLAY_DEVICE device = {};
		device.cb = sizeof( device );
		if ( !EnumDisplayDevices( NULL, index, &device, 0 ) ) {
			break;
		}
		if ( s_lastFullscreen != 0 && s_lastFullscreenDevice == device.DeviceName &&
			WIN_GetFullscreenDevice( index + 1, device ) ) {
			// An available remembered display with an invalid mode is not replaced.
			return WIN_CanRestoreFullscreen( index + 1 ) ? index + 1 : 0;
		}
		if ( WIN_CanRestoreFullscreen( index + 1 ) &&
			( fallback == 0 || idStr::Icmp( nearest.szDevice, device.DeviceName ) == 0 ) ) {
			fallback = index + 1;
		}
	}
	if ( fallback != 0 ) {
		idLib::Printf( "Alt+Enter: %s; using display %d with the current video settings.\n",
			s_lastFullscreen == 0 ? "no fullscreen history" : "previous display unavailable", fallback );
	}
	return fallback;
}

CONSOLE_COMMAND_SHIP( winToggleFullscreen, "internal queued Alt+Enter action", NULL ) {
	if ( !renderSystem->IsInitialized() || win32.hWnd == NULL ||
		s_fullscreenPreview || s_fullscreenRestart ) {
		return;
	}
	// Execute at the command's queue position, not while Windows is dispatching
	// keys. Several deliberate presses then observe each preceding restart.
	const int requested = cvarSystem->GetCVarInteger( "r_fullscreen" );
	const int target = requested == 0 ? WIN_FullscreenTarget() : 0;
	if ( requested == 0 && target == 0 ) {
		idLib::Printf( "Alt+Enter: no valid fullscreen target for the current video settings.\n" );
		return;
	}
	WIN_RememberFullscreen();
	struct restartGuard_t {
		restartGuard_t() { s_fullscreenRestart = true; }
		~restartGuard_t() { s_fullscreenRestart = false; }
	} guard;
	cvarSystem->SetCVarInteger( "r_fullscreen", target );
	cmdSystem->BufferCommandText( CMD_EXEC_NOW, "vid_restart\n" );
	WIN_RememberFullscreen();
}

static void WIN_DisableAltTab() {
	if ( s_alttab_disabled || win_allowAltTab.GetBool() ) {
		return;
	}
	if ( !idStr::Icmp( cvarSystem->GetCVarString( "sys_arch" ), "winnt" ) ) {
		RegisterHotKey( 0, 0, MOD_ALT, VK_TAB );
	} else {
		BOOL old;

		SystemParametersInfo( SPI_SCREENSAVERRUNNING, 1, &old, 0 );
	}
	s_alttab_disabled = true;
}

static void WIN_EnableAltTab() {
	if ( !s_alttab_disabled || win_allowAltTab.GetBool() ) {
		return;
	}
	if ( !idStr::Icmp( cvarSystem->GetCVarString( "sys_arch" ), "winnt" ) ) {
		UnregisterHotKey( 0, 0 );
	} else {
		BOOL old;

		SystemParametersInfo( SPI_SCREENSAVERRUNNING, 0, &old, 0 );
	}

	s_alttab_disabled = false;
}

void WIN_Sizing(WORD side, RECT *rect)
{
	if ( !renderSystem->IsInitialized() || renderSystem->GetWidth() <= 0 || renderSystem->GetHeight() <= 0 ) {
		return;
	}

	// restrict to a standard aspect ratio
	int width = rect->right - rect->left;
	int height = rect->bottom - rect->top;

	// Adjust width/height for window decoration
	RECT decoRect = { 0, 0, 0, 0 };
	AdjustWindowRect( &decoRect, WINDOW_STYLE|WS_SYSMENU, FALSE );
	int decoWidth = decoRect.right - decoRect.left;
	int decoHeight = decoRect.bottom - decoRect.top;

	width -= decoWidth;
	height -= decoHeight;

	// Clamp to a minimum size
	if ( width < SCREEN_WIDTH / 4 ) {
		width = SCREEN_WIDTH / 4;
	}
	if ( height < SCREEN_HEIGHT / 4 ) {
		height = SCREEN_HEIGHT / 4;
	}

	const int minWidth = height * 4 / 3;
	const int maxHeight = width * 3 / 4;

	const int maxWidth = height * 16 / 9;
	const int minHeight = width * 9 / 16;

	// Set the new size
	switch ( side ) {
	case WMSZ_LEFT:
		rect->left = rect->right - width - decoWidth;
		rect->bottom = rect->top + idMath::ClampInt( minHeight, maxHeight, height ) + decoHeight;
		break;
	case WMSZ_RIGHT:
		rect->right = rect->left + width + decoWidth;
		rect->bottom = rect->top + idMath::ClampInt( minHeight, maxHeight, height ) + decoHeight;
		break;
	case WMSZ_BOTTOM:
	case WMSZ_BOTTOMRIGHT:
		rect->bottom = rect->top + height + decoHeight;
		rect->right = rect->left + idMath::ClampInt( minWidth, maxWidth, width ) + decoWidth;
		break;
	case WMSZ_TOP:
	case WMSZ_TOPRIGHT:
		rect->top = rect->bottom - height - decoHeight;
		rect->right = rect->left + idMath::ClampInt( minWidth, maxWidth, width ) + decoWidth;
		break;
	case WMSZ_BOTTOMLEFT:
		rect->bottom = rect->top + height + decoHeight;
		rect->left = rect->right - idMath::ClampInt( minWidth, maxWidth, width ) - decoWidth;
		break;
	case WMSZ_TOPLEFT:
		rect->top = rect->bottom - height - decoHeight;
		rect->left = rect->right - idMath::ClampInt( minWidth, maxWidth, width ) - decoWidth;
		break;
	}
}

/*
====================
MainWndProc

main window procedure
====================
*/
LRESULT CALLBACK MainWndProc( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam ) {
	int key;
	switch( uMsg ) {
		case WM_WINDOWPOSCHANGED:
			if ( renderSystem->IsInitialized() ) {
				RECT rect;
				if (::GetClientRect( win32.hWnd, &rect ) ) {

					if ( rect.right > rect.left && rect.bottom > rect.top ) {
						win32.nativeScreenWidth = rect.right - rect.left;
						win32.nativeScreenHeight = rect.bottom - rect.top;
					}
				}
			}
			break;
		case WM_MOVE: {
			int		xPos, yPos;
			RECT r;

			// save the window origin in cvars if we aren't fullscreen
			int style = GetWindowLong( hWnd, GWL_STYLE );
			if ( ( style & WS_POPUP ) == 0 ) {
				xPos = (short) LOWORD(lParam);    // horizontal position 
				yPos = (short) HIWORD(lParam);    // vertical position 

				r.left   = 0;
				r.top    = 0;
				r.right  = 1;
				r.bottom = 1;

				AdjustWindowRect( &r, style, FALSE );

				r_windowX.SetInteger( xPos + r.left );
				r_windowY.SetInteger( yPos + r.top );
			}
			break;
		}
		case WM_CREATE:

			win32.hWnd = hWnd;

			if ( win32.cdsFullscreen ) {
				WIN_DisableAltTab();
			} else {
				WIN_EnableAltTab();
			}

			break;

		case WM_DESTROY:
			// let sound and input know about this?
			win32.hWnd = NULL;
			if ( win32.cdsFullscreen ) {
				WIN_EnableAltTab();
			}
			break;

		case WM_CLOSE:
			I_ClassicAudioEvent( "WM_CLOSE" );
			soundSystem->SetMute( true );
			cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "quit\n" );
			break;

		case WM_ACTIVATE:
			// if we got here because of an alt-tab or maximize,
			// we should activate immediately.  If we are here because
			// the mouse was clicked on a title bar or drag control,
			// don't activate until the mouse button is released
			{
				int	fActive, fMinimized;

				fActive = LOWORD(wParam);
				fMinimized = (BOOL) HIWORD(wParam);

				I_ClassicAudioEvent( "WM_ACTIVATE", win32.activeApp, fActive != WA_INACTIVE,
					fMinimized ? "minimized" : "not-minimized" );
				win32.activeApp = (fActive != WA_INACTIVE);
				if ( win32.activeApp ) {
					idKeyInput::ClearStates();
					Sys_GrabMouseCursor( true );
					if ( common->IsInitialized() ) {
						SetCursor( NULL );
					}
				}

				if ( fActive == WA_INACTIVE ) {
					win32.movingWindow = false;
					if ( common->IsInitialized() ) {
						SetCursor( LoadCursor( 0, IDC_ARROW ) );
					}
				}

				// start playing the game sound world
				soundSystem->SetMute( !win32.activeApp );

				// we do not actually grab or release the mouse here,
				// that will be done next time through the main loop
			}
			break;
		case WM_TIMER: {
			if ( win_timerUpdate.GetBool() ) {
				common->Frame();
			}
			break;
		}
		case WM_SYSCOMMAND:
			if ( wParam == SC_SCREENSAVE || wParam == SC_KEYMENU ) {
				return 0;
			}
			break;

		case WM_SYSKEYDOWN:
			if ( wParam == VK_RETURN && ( lParam & ( LPARAM( 1 ) << 29 ) ) != 0 ) {
				// Windows supplies the previous key state across focus/HWND changes.
				// No key-up latch: a lost release cannot disable the next real press.
				if ( ( lParam & ( LPARAM( 1 ) << 30 ) ) == 0 && !s_fullscreenPreview ) {
					cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "winToggleFullscreen\n" );
				}
				return 0;
			}
			// fall through for other keys
		case WM_KEYDOWN:
			key = ( ( lParam >> 16 ) & 0xFF ) | ( ( ( lParam >> 24 ) & 1 ) << 7 );
			if ( key == K_LCTRL || key == K_LALT || key == K_RCTRL || key == K_RALT ) {
				// let direct-input handle this because windows sends Alt-Gr
				// as two events (ctrl then alt)
				break;
			}
			// D
			if ( key == K_NUMLOCK ) {
				key = K_PAUSE;
			} else if ( key == K_PAUSE ) {
				key = K_NUMLOCK;
			}
			Sys_QueEvent( SE_KEY, key, true, 0, NULL, 0 );
			break;

		case WM_SYSKEYUP:
		case WM_KEYUP:
			key = ( ( lParam >> 16 ) & 0xFF ) | ( ( ( lParam >> 24 ) & 1 ) << 7 );
			if ( key == K_PRINTSCREEN ) {
				// don't queue printscreen keys.  Since windows doesn't send us key
				// down events for this, we handle queueing them with DirectInput
				break;
			} else if ( key == K_LCTRL || key == K_LALT || key == K_RCTRL || key == K_RALT ) {
				// let direct-input handle this because windows sends Alt-Gr
				// as two events (ctrl then alt)
				break;
			}
			Sys_QueEvent( SE_KEY, key, false, 0, NULL, 0 );
			break;

		case WM_CHAR:
			Sys_QueEvent( SE_CHAR, wParam, 0, 0, NULL, 0 );
			break;

		case WM_NCLBUTTONDOWN:
//			win32.movingWindow = true;
			break;

		case WM_ENTERSIZEMOVE:
			win32.movingWindow = true;
			break;

		case WM_EXITSIZEMOVE:
			win32.movingWindow = false;
			break;

		case WM_SIZING:
			WIN_Sizing(wParam, (RECT *)lParam);
			break;
		case WM_MOUSEMOVE: {
			if ( !common->IsInitialized() ) {
				break;
			}

			const bool isShellActive = ( game && ( game->Shell_IsActive() || game->IsPDAOpen() ) );
			const bool isConsoleActive = console->Active();

			if ( win32.activeApp ) {
				if ( isShellActive ) {
					// If the shell is active, it will display its own cursor.
					SetCursor( NULL );
				} else if ( isConsoleActive ) {
					// The console is active but the shell is not.
					// Show the Windows cursor.
					SetCursor( LoadCursor( 0, IDC_ARROW ) );
				} else {
					// The shell not active and neither is the console.
					// This is normal gameplay, hide the cursor.
					SetCursor( NULL );
				}
			} else {
				if ( !isShellActive ) {
					// Always show the cursor when the window is in the background
					SetCursor( LoadCursor( 0, IDC_ARROW ) );
				} else {
					SetCursor( NULL );
				}
			}

			const int x = GET_X_LPARAM( lParam );
			const int y = GET_Y_LPARAM( lParam );

			// Generate an event
			Sys_QueEvent( SE_MOUSE_ABSOLUTE, x, y, 0, NULL, 0 );
			
			// Get a mouse leave message
			TRACKMOUSEEVENT tme = {
				sizeof( TRACKMOUSEEVENT ),
				TME_LEAVE,
				hWnd,
				0
			};
			
			TrackMouseEvent( &tme );

			return 0;
		}
		case WM_MOUSELEAVE: {
			Sys_QueEvent( SE_MOUSE_LEAVE, 0, 0, 0, NULL, 0 );
			return 0;
		}
		case WM_LBUTTONDOWN: {
			Sys_QueEvent( SE_KEY, K_MOUSE1, 1, 0, NULL, 0 );
			return 0;
		}
		case WM_LBUTTONUP: {
			Sys_QueEvent( SE_KEY, K_MOUSE1, 0, 0, NULL, 0 );
			return 0;
		}
		case WM_RBUTTONDOWN: {
			Sys_QueEvent( SE_KEY, K_MOUSE2, 1, 0, NULL, 0 );
			return 0;
		}
		case WM_RBUTTONUP: {
			Sys_QueEvent( SE_KEY, K_MOUSE2, 0, 0, NULL, 0 );
			return 0;
		}
		case WM_MBUTTONDOWN: {
			Sys_QueEvent( SE_KEY, K_MOUSE3, 1, 0, NULL, 0 );
			return 0;
		}
		case WM_MBUTTONUP: {
			Sys_QueEvent( SE_KEY, K_MOUSE3, 0, 0, NULL, 0 );
			return 0;
		}
		case WM_XBUTTONDOWN: {
			int button = GET_XBUTTON_WPARAM( wParam );
			if ( button == 1 ) {
				Sys_QueEvent( SE_KEY, K_MOUSE4, 1, 0, NULL, 0 );
			} else if ( button == 2 ) {
				Sys_QueEvent( SE_KEY, K_MOUSE5, 1, 0, NULL, 0 );
			}
			return 0;
		}
		case WM_XBUTTONUP: {
			int button = GET_XBUTTON_WPARAM( wParam );
			if ( button == 1 ) {
				Sys_QueEvent( SE_KEY, K_MOUSE4, 0, 0, NULL, 0 );
			} else if ( button == 2 ) {
				Sys_QueEvent( SE_KEY, K_MOUSE5, 0, 0, NULL, 0 );
			}
			return 0;
		}
		case WM_MOUSEWHEEL: {
			int delta = GET_WHEEL_DELTA_WPARAM( wParam ) / WHEEL_DELTA;
			key = delta < 0 ? K_MWHEELDOWN : K_MWHEELUP;
			delta = abs( delta );
			while( delta-- > 0 ) {
				Sys_QueEvent( SE_KEY, key, true, 0, NULL, 0 );
				Sys_QueEvent( SE_KEY, key, false, 0, NULL, 0 );
			}
			break;
		}
	}

    return DefWindowProc( hWnd, uMsg, wParam, lParam );
}
