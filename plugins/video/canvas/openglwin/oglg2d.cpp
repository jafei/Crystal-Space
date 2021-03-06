/*
    Copyright (C) 1998 by Jorrit Tyberghein

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public
    License along with this library; if not, write to the Free
    Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "cssysdef.h"
#include "csutil/sysfunc.h"


#include "csplugincommon/win32/icontools.h"
#include "csutil/win32/wintools.h"
#include "csutil/win32/win32.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>

#include "csutil/scf.h"
#include "oglg2d.h"
#include "iutil/objreg.h"
#include "ivaria/reporter.h"

#include "iutil/cmdline.h"
#include "iutil/eventq.h"

#include "csutil/win32/psdk-compat.h"

#ifndef GL_VERSION_1_1
#error OpenGL version 1.1 required! Stopping compilation.
#endif

#include "DwmapiAPI.h"

/*
    In fs mode, the window is topmost, means above every other
    window, all the time. But when debugging it is really annoying to 
    have a black window in front of your face instead of the IDE... 
    Note: this hack may cause taskbar flickering when "always on top" is
    enabled and auto-hide is disabled for the task bar.
 */
#ifdef CS_DEBUG
# define CS_WINDOW_Z_ORDER HWND_TOP
#else
# define CS_WINDOW_Z_ORDER HWND_TOPMOST
#endif

static void SystemFatalError (const wchar_t* str, HRESULT hRes = ~0)
{
  wchar_t* newMsg = 0;
  const wchar_t* szMsg;
  const wchar_t szStdMessage[] = L"\nLast Error: ";

  if (hRes != ~0)
  {
    wchar_t* lpMsgBuf = cswinGetErrorMessageW (hRes);

    szMsg = newMsg = new wchar_t[wcslen (lpMsgBuf) + wcslen (str)
      + wcslen (szStdMessage) + 1];
    wcscpy (newMsg, str);
    wcscat (newMsg, szStdMessage);
    wcscat (newMsg, lpMsgBuf);
  
    delete[] lpMsgBuf ;
  }
  else
    szMsg = str;

  MessageBoxW (0, szMsg, L"Fatal Error in glwin32.dll", 
    MB_OK | MB_ICONERROR);

  delete[] newMsg;

  exit(1);
}

/////The 2D Graphics Driver//////////////

SCF_IMPLEMENT_FACTORY (csGraphics2DOpenGL)

csGraphics2DOpenGL::csGraphics2DOpenGL (iBase *iParent) :
  scfImplementationType (this, iParent),
  m_nGraphicsReady (true),
  m_hWnd (0),
  primaryMonitor (0),
  modeSwitched (true),
  customIcon (0),
  transparencyRequested (false),
  transparencyState (false),
  hideDecoClientFrame (false)
{
  Dwmapi::IncRef ();
}

csGraphics2DOpenGL::~csGraphics2DOpenGL (void)
{
  m_nGraphicsReady = 0;
  Dwmapi::DecRef ();
}

void csGraphics2DOpenGL::Report (int severity, const char* msg, ...)
{
  va_list arg;
  va_start (arg, msg);
  csRef<iReporter> rep (csQueryRegistry<iReporter> (object_reg));
  if (rep)
    rep->ReportV (severity, "crystalspace.canvas.openglwin", msg, arg);
  else
  {
    csPrintfV (msg, arg);
    csPrintf ("\n");
  }
  va_end (arg);
}

bool csGraphics2DOpenGL::Initialize (iObjectRegistry *object_reg)
{
  if (!csGraphics2DGLCommon::Initialize (object_reg))
    return false;

  m_piWin32Assistant = csQueryRegistry<iWin32Assistant> (object_reg);
  if (!m_piWin32Assistant)
    SystemFatalError (L"csGraphics2DOpenGL::Open(QI) -- system passed does not support iWin32Assistant.");

  // Get the creation parameters
  m_hInstance = m_piWin32Assistant->GetInstance ();
  m_nCmdShow  = m_piWin32Assistant->GetCmdShow ();

  // store a copy of the refresh rate as we may need it later
  m_nDisplayFrequency = refreshRate;

  return true;
}

int csGraphics2DOpenGL::FindPixelFormatGDI (HDC hDC, 
					    csGLPixelFormatPicker& picker)
{
  int newPixelFormat = 0;
  GLPixelFormat& format = currentFormat;
  while (picker.GetNextFormat (format))
  {
    PIXELFORMATDESCRIPTOR pfd = {
	sizeof(PIXELFORMATDESCRIPTOR),  /* size */
	1,                              /* version */
	PFD_SUPPORT_OPENGL |
	PFD_DOUBLEBUFFER |              /* support double-buffering */
	PFD_DRAW_TO_WINDOW,
	PFD_TYPE_RGBA,                  /* color type */
	format[glpfvColorBits],         /* prefered color depth */
	0, 0, 0, 0, 0, 0,               /* color bits (ignored) */
	format[glpfvAlphaBits],         /* no alpha buffer */
	0,                              /* alpha bits (ignored) */
	format[glpfvAccumColorBits],	/* accumulation buffer */
	0, 0, 0,			/* accum bits (ignored) */
	format[glpfvAccumAlphaBits],	/* accum alpha bits */
	format[glpfvDepthBits],         /* depth buffer */
	format[glpfvStencilBits],	/* stencil buffer */
	0,                              /* no auxiliary buffers */
	PFD_MAIN_PLANE,                 /* main layer */
	0,                              /* reserved */
	0, 0, 0                         /* no layer, visible, damage masks */
    };

    newPixelFormat = ChoosePixelFormat (hDC, &pfd);

    if (newPixelFormat == 0)
      SystemFatalError (L"ChoosePixelFormat failed.", GetLastError());

    if (DescribePixelFormat (hDC, newPixelFormat, 
      sizeof(PIXELFORMATDESCRIPTOR), &pfd) == 0)
      SystemFatalError (L"DescribePixelFormat failed.", GetLastError());

    if (!(pfd.dwFlags & PFD_GENERIC_FORMAT) ||
      (pfd.dwFlags & PFD_GENERIC_ACCELERATED))
    {
      return newPixelFormat;
    }
  }

  return newPixelFormat;
}

int csGraphics2DOpenGL::FindPixelFormatWGL (csGLPixelFormatPicker& picker)
{
  /*
    To use multisampling, a special pixel format has to determined.
    However, this determination works over a WGL ext - thus we need
    a GL context. So we create a window just for checking that
    ext.
   */
  static const char* dummyClassName = "CSGL_DummyWindow";

  HINSTANCE ModuleHandle = GetModuleHandle(0);

  WNDCLASSA wc;
  wc.hCursor        = 0;
  wc.hIcon	    = 0;
  wc.lpszMenuName   = 0;
  wc.lpszClassName  = dummyClassName;
  wc.hbrBackground  = (HBRUSH)(COLOR_BTNFACE + 1);
  wc.hInstance      = ModuleHandle;
  wc.style          = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
  wc.lpfnWndProc    = DummyWindow;
  wc.cbClsExtra     = 0;
  wc.cbWndExtra     = 0;

  if (!RegisterClassA (&wc)) return false;

  DummyWndInfo dwi;
  dwi.pixelFormat = -1;
  dwi.this_ = this;
  dwi.chosenFormat = &currentFormat;
  dwi.picker = &picker;

  HWND wnd = CreateWindowA (dummyClassName, 0, 0, CW_USEDEFAULT, 
    CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, 0, 0,
    ModuleHandle, (LPVOID)&dwi);
  DestroyWindow (wnd);

  UnregisterClassA (dummyClassName, ModuleHandle);

  ext.Reset();

  return dwi.pixelFormat;
}

LRESULT CALLBACK csGraphics2DOpenGL::DummyWindow (HWND hWnd, UINT message,
  WPARAM wParam, LPARAM lParam)
{
  switch(message)
  {
  case WM_CREATE:
    {
      DummyWndInfo* dwi = (DummyWndInfo*)(LPCREATESTRUCT(lParam)->lpCreateParams);

      HDC hDC = GetDC (hWnd);
      int acceleration = dwi->this_->config->GetBool (
	"Video.OpenGL.FullAcceleration", true) ? WGL_FULL_ACCELERATION_ARB : 
	WGL_GENERIC_ACCELERATION_ARB;
      csGLPixelFormatPicker& picker = *dwi->picker;

      int pixelFormat = dwi->this_->FindPixelFormatGDI (hDC, picker);
      PIXELFORMATDESCRIPTOR pfd;
      if (DescribePixelFormat (hDC, pixelFormat, 
	sizeof(PIXELFORMATDESCRIPTOR), &pfd) == 0)
	SystemFatalError (L"DescribePixelFormat failed.");

      if (SetPixelFormat (hDC, pixelFormat, 0) != TRUE)
      {
	HRESULT spfErr = (HRESULT)GetLastError();
	SystemFatalError (L"SetPixelFormat failed.", spfErr);
      }

      HGLRC hGLRC = wglCreateContext (hDC);
      wglMakeCurrent (hDC, hGLRC);

      csGLExtensionManager& ext = dwi->this_->ext;
      ext.Open();

      dwi->this_->detector.DoDetection (hWnd, hDC);
      dwi->this_->OpenDriverDB ("preinit");

      ext.InitWGL_ARB_pixel_format (hDC);
      if (ext.CS_WGL_ARB_pixel_format)
      {
	unsigned int numFormats = 0;
	int iAttributes[26];
	float fAttributes[] = {0.0f, 0.0f};

	GLPixelFormat format;
	memcpy (format, dwi->chosenFormat, sizeof (GLPixelFormat));
	do
	{
	  int index = 0;
	  iAttributes[index++] = WGL_DRAW_TO_WINDOW_ARB;
	  iAttributes[index++] = GL_TRUE;
	  iAttributes[index++] = WGL_SUPPORT_OPENGL_ARB;
	  iAttributes[index++] = GL_TRUE;
	  iAttributes[index++] = WGL_ACCELERATION_ARB;
	  iAttributes[index++] = acceleration;
	  iAttributes[index++] = WGL_DOUBLE_BUFFER_ARB;
	  iAttributes[index++] = GL_TRUE;
	  iAttributes[index++] = WGL_SAMPLE_BUFFERS_ARB;
	  iAttributes[index++] = 
	    (format[glpfvMultiSamples] != 0) ? 1 : 0;
	  iAttributes[index++] = WGL_SAMPLES_ARB;
	  iAttributes[index++] = format[glpfvMultiSamples];
	  iAttributes[index++] = WGL_COLOR_BITS_ARB;
	  iAttributes[index++] = pfd.cColorBits;
  	  iAttributes[index++] = WGL_ALPHA_BITS_ARB;
  	  iAttributes[index++] = pfd.cAlphaBits;
  	  iAttributes[index++] = WGL_DEPTH_BITS_ARB;
	  iAttributes[index++] = pfd.cDepthBits;
	  iAttributes[index++] = WGL_STENCIL_BITS_ARB;
	  iAttributes[index++] = pfd.cStencilBits;	  
	  iAttributes[index++] = WGL_ACCUM_BITS_ARB;
	  iAttributes[index++] = pfd.cAccumBits;
	  iAttributes[index++] = WGL_ACCUM_ALPHA_BITS_ARB;
	  iAttributes[index++] = pfd.cAccumAlphaBits;
	  iAttributes[index++] = 0;
	  iAttributes[index++] = 0;

	  if ((ext.wglChoosePixelFormatARB (hDC, iAttributes, fAttributes,
	    1, &dwi->pixelFormat, &numFormats) == GL_TRUE) && (numFormats != 0))
	  {
	    int queriedAttrs[] = {WGL_SAMPLES_ARB};
	    const int queriedAttrNum = sizeof(queriedAttrs) / sizeof(int);
	    int attrValues[queriedAttrNum];

	    if (ext.wglGetPixelFormatAttribivARB (hDC, dwi->pixelFormat, 0,
	      queriedAttrNum, queriedAttrs, attrValues) == GL_TRUE)
	    {
	      (*dwi->chosenFormat)[glpfvMultiSamples] = attrValues[0];
	      break;
	    }
	  }
	}
	while (picker.GetNextFormat (format));
      }

      dwi->this_->driverdb.Close ();

      wglMakeCurrent (hDC, 0);
      wglDeleteContext (hGLRC);

      ReleaseDC (hWnd, hDC);
    }
    break;
  }
  return DefWindowProc (hWnd, message, wParam, lParam);
}

bool csGraphics2DOpenGL::Open ()
{
  if (is_open) return true;

  csRef<iVerbosityManager> verbosemgr (
    csQueryRegistry<iVerbosityManager> (object_reg));
  if (verbosemgr) 
    detector.SetVerbose (verbosemgr->Enabled ("renderer.windows.gldriver"));
  
  // create the window.
  if (FullScreen)
  {
    SwitchDisplayMode (false);
  }

  int pixelFormat = -1;
  csGLPixelFormatPicker picker (this);
  /*
    Check if the WGL pixel format check should be used at all.
    It appears that some drivers take "odd" choices when using the WGL
    pixel format path (e.g. returning Accum-capable formats even if none
    was requested).
   */
  bool doWGLcheck = false;
  {
    GLPixelFormat format;
    if (picker.GetNextFormat (format))
    {
      doWGLcheck = (format[glpfvMultiSamples] != 0);
      picker.Reset ();
    }
  }
  if (doWGLcheck)
    pixelFormat = FindPixelFormatWGL (picker);

  m_bActivated = true;

  RECT wndRect;
  DWORD exStyle = 0;
  DWORD style = WS_POPUP | WS_SYSMENU;
  windowModeStyle = WS_CAPTION;
  if (FullScreen)
  {
    /*exStyle |= WS_EX_TOPMOST;*/
    wndRect.left = 0;
    wndRect.top = 0;
    wndRect.right = fbWidth;
    wndRect.bottom = fbHeight;
  }
  else
  {
    style |= windowModeStyle | WS_MINIMIZEBOX;
    if (AllowResizing) 
      style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
    
    ComputeDefaultRect (wndRect, style, exStyle);
  }

  m_hWnd = m_piWin32Assistant->CreateCSWindow (this, exStyle, style,
    wndRect.left, wndRect.top,
    wndRect.right - wndRect.left, wndRect.bottom - wndRect.top);

  if (!m_hWnd)
    SystemFatalError (L"Cannot create Crystal Space window", GetLastError());

  SetTitle (win_title);
  
  // Subclass the window
  if (IsWindowUnicode (m_hWnd))
  {
    m_OldWndProc = (WNDPROC)SetWindowLongPtrW (m_hWnd, GWLP_WNDPROC, 
      (LONG_PTR) WindowProc);
    SetWindowLongPtrW (m_hWnd, GWLP_USERDATA, (LONG_PTR)this);
  }
  else
  {
    m_OldWndProc = (WNDPROC)SetWindowLongPtrA (m_hWnd, GWLP_WNDPROC, 
      (LONG_PTR) WindowProc);
    SetWindowLongPtrA (m_hWnd, GWLP_USERDATA, (LONG_PTR)this);
  }

  hDC = GetDC (m_hWnd);
  if (pixelFormat == -1)
  {
    picker.Reset();
    pixelFormat = FindPixelFormatGDI (hDC, picker);
  }

  PIXELFORMATDESCRIPTOR pfd;
  if (DescribePixelFormat (hDC, pixelFormat, 
    sizeof(PIXELFORMATDESCRIPTOR), &pfd) == 0)
    SystemFatalError (L"DescribePixelFormat failed.", GetLastError());

  if (SetPixelFormat (hDC, pixelFormat, &pfd) != TRUE)
  {
    HRESULT spfErr = (HRESULT)GetLastError();
    SystemFatalError (L"SetPixelFormat failed.", spfErr);
  }

  currentFormat[glpfvColorBits] = pfd.cColorBits;
  currentFormat[glpfvAlphaBits] = pfd.cAlphaBits;
  currentFormat[glpfvDepthBits] = pfd.cDepthBits;
  currentFormat[glpfvStencilBits] = pfd.cStencilBits;
  currentFormat[glpfvAccumColorBits] = pfd.cAccumBits;
  currentFormat[glpfvAccumAlphaBits] = pfd.cAccumAlphaBits;

  Depth = pfd.cColorBits; 

  hardwareAccelerated = !(pfd.dwFlags & PFD_GENERIC_FORMAT) ||
    (pfd.dwFlags & PFD_GENERIC_ACCELERATED);

  hGLRC = wglCreateContext (hDC);
  wglMakeCurrent (hDC, hGLRC);

  UpdateWindow (m_hWnd);
  ShowWindow (m_hWnd, m_nCmdShow);
  SetForegroundWindow (m_hWnd);
  SetFocus (m_hWnd);
  
  /* Small hack to emit "no HW acceleration" message on both GDI Generic and
   * sucky Direct3D default OpenGL */
  hardwareAccelerated &= 
    (strncmp ((char*)glGetString (GL_VENDOR), "Microsoft", 9) != 0);
  if (!hardwareAccelerated)
  {
    Report (CS_REPORTER_SEVERITY_WARNING,
      "No hardware acceleration!");
  }

  detector.DoDetection (m_hWnd, hDC);
  Report (CS_REPORTER_SEVERITY_NOTIFY,
    "GL driver: %s %s", detector.GetDriverDLL(), 
    detector.GetDriverVersion() ? detector.GetDriverVersion() : 
      "<version unknown>");

  if (FullScreen)
  {
    /* 
     * from the Windows Shell docs:
     * "It is possible to cover the taskbar by explicitly setting the size 
     * of the window rectangle equal to the size of the screen with 
     * SetWindowPos."
     */
    SetWindowPos (m_hWnd, CS_WINDOW_Z_ORDER, 0, 0, fbWidth, fbHeight, 0);
  }

  if (!csGraphics2DGLCommon::Open ())
    return false;

  ext.InitWGL_EXT_swap_control (hDC);

  if (ext.CS_WGL_EXT_swap_control)
  {
    ext.wglSwapIntervalEXT (vsync ? 1 : 0);
    vsync = (ext.wglGetSwapIntervalEXT() != 0);
    Report (CS_REPORTER_SEVERITY_NOTIFY,
      "VSync is %s.", 
      vsync ? "enabled" : "disabled");
  }

  hideDecoClientFrame = false;
  // Set transparency, if any was requested
  csGraphics2DOpenGL::SetWindowTransparent (transparencyRequested);

  return true;
}

bool csGraphics2DOpenGL::RestoreDisplayMode ()
{
  if (is_open)
  {
    if (FullScreen) SwitchDisplayMode (true);
    return true;
  }
  return false;
}

const char* csGraphics2DOpenGL::GetRendererString (const char* str)
{
  if (strcmp (str, "win32_driver") == 0)
  {
    return detector.GetDriverDLL();
  }
  else if (strcmp (str, "win32_driverversion") == 0)
  {
    return detector.GetDriverVersion();
  }
  else
    return csGraphics2DGLCommon::GetRendererString (str);
}

const char* csGraphics2DOpenGL::GetVersionString (const char* ver)
{
  if (strcmp (ver, "win32_driver") == 0)
  {
    return detector.GetDriverVersion();
  }
  else
    return csGraphics2DGLCommon::GetVersionString (ver);
}

void csGraphics2DOpenGL::Close (void)
{
  if (!is_open) return;
  
  csGraphics2DGLCommon::Close ();

  if (hGLRC)
  {
    wglMakeCurrent (hDC, 0);
    wglDeleteContext (hGLRC);
  }

  ReleaseDC (m_hWnd, hDC);

  if (m_hWnd != 0)
    DestroyWindow (m_hWnd);

  RestoreDisplayMode ();
}

void csGraphics2DOpenGL::Print (csRect const* /*area*/)
{
  SwapBuffers(hDC);
}

bool csGraphics2DOpenGL::SetMouseCursor (csMouseCursorID iShape)
{
  csRef<iWin32Assistant> winhelper (
  	csQueryRegistry<iWin32Assistant> (object_reg));
  if (winhelper == 0) return false;
  bool rc;
  if (hwMouse == hwmcOff)
  {
    winhelper->SetCursor (csmcNone);
    rc = false;
  }
  else
  {
    rc = winhelper->SetCursor (iShape);
  }
  return rc;
}

bool csGraphics2DOpenGL::SetMouseCursor (iImage *image, const csRGBcolor* keycolor, 
					 int hotspot_x, int hotspot_y,
					 csRGBcolor fg, csRGBcolor bg)
{
  if (hwMouse == hwmcOff)
  {
    m_piWin32Assistant->SetCursor (csmcNone);
    return false;
  }
  HCURSOR cur = cursors.GetMouseCursor (image, keycolor, hotspot_x, 
    hotspot_y, fg, bg);
  if (cur == 0)
  {
    m_piWin32Assistant->SetCursor (csmcNone);
    return false;
  }
  return m_piWin32Assistant->SetHCursor (cur);
}

bool csGraphics2DOpenGL::SetMousePosition (int x, int y)
{
  POINT p;

  p.x = x;
  p.y = y;

  ClientToScreen (m_hWnd, &p);

  ::SetCursorPos (p.x, p.y);

  return true;
}

bool csGraphics2DOpenGL::PerformExtensionV (char const* command, va_list args)
{
  if (!strcasecmp (command, "hardware_accelerated"))
  {
    bool* hasAccel = (bool*)va_arg (args, bool*);
    *hasAccel = hardwareAccelerated;
    return true;
  }
  if (!strcasecmp (command, "configureopengl"))
  {
    // Ugly hack needed to work around an interference between the 3dfx opengl
    // driver on voodoo cards <= 2 and the win32 console window
    if (GetFullScreen() && config->GetBool (
    	"Video.OpenGL.Win32.DisableConsoleWindow", false) )
    {
      m_piWin32Assistant->DisableConsole ();
      Report (CS_REPORTER_SEVERITY_NOTIFY,
      	"*** Disabled Win32 console window to avoid OpenGL interference.");
    }
    csGraphics2DGLCommon::PerformExtensionV (command, args);
    return true;
  }
  if (!strcasecmp (command, "getcoords"))
  {
    csRect* r = (csRect*)va_arg (args, csRect*);
    RECT wr;
    GetWindowRect (m_hWnd, &wr);
    r->Set (wr.left, wr.top, wr.right, wr.bottom);
    return true;
  }
  if (!strcasecmp (command, "setcoords"))
  {
    if (!AllowResizing) return false;
    csRect* r = (csRect*)va_arg (args, csRect*);
    SetWindowPos (m_hWnd, 0, r->xmin, r->ymin, r->Width(), r->Height(),
      SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
  }
  if (!strcasecmp (command, "setglcontext"))
  {
    wglMakeCurrent (hDC, hGLRC);
    return true;
  }
  return csGraphics2DGLCommon::PerformExtensionV (command, args);
}

void csGraphics2DOpenGL::SetTitle (const char* title)
{
  csGraphics2D::SetTitle (title);
  if (m_hWnd)
  {
    if (IsWindowUnicode (m_hWnd))
      SetWindowTextW (m_hWnd, csCtoW (title));
    else
      SetWindowTextA (m_hWnd, cswinCtoA (title));
  }
}

void csGraphics2DOpenGL::SetIcon (iImage *image)
{
  HICON icon = CS::Platform::Win32::IconTools::IconFromImage (image);
  SendMessage (m_hWnd, WM_SETICON, ICON_BIG, (LPARAM)icon);
  if (customIcon != 0) DestroyIcon (customIcon);
  customIcon = icon;
}

void csGraphics2DOpenGL::AlertV (int type, const char* title, const char* okMsg,
	const char* msg, va_list arg)
{
  m_piWin32Assistant->AlertV (m_hWnd, type, title, okMsg, msg, arg);
}

void csGraphics2DOpenGL::AllowResize (bool iAllow)
{
  if (FullScreen)
  {
    return;
  }
  else
  {
    if (AllowResizing != iAllow)
    {
      LONG style = GetWindowLong (m_hWnd,
	GWL_STYLE);
      RECT R;

      GetClientRect (m_hWnd, &R);
      ClientToScreen (m_hWnd, (LPPOINT)&R.left);
      ClientToScreen (m_hWnd, (LPPOINT)&R.right);

      AllowResizing = iAllow;
      if (AllowResizing)
      {
	style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
      }
      else
      {
	style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
      }
      SetWindowLong (m_hWnd, GWL_STYLE, style);

      AdjustWindowRectEx (&R, style, false, 0);
      SetWindowPos (m_hWnd, 0, R.left, R.top, R.right - R.left,
	R.bottom - R.top, SWP_NOZORDER | SWP_DRAWFRAME);
    }
  }
}

bool csGraphics2DOpenGL::CanvasResize (int width, int height)
{
  if (!CS::PluginCommon::GL::CanvasCommonBase::CanvasResize (width, height))
    return false;

  if (is_open && !FullScreen)
  {
    RECT R;
    GetClientRect (m_hWnd, &R);
    if (R.right - R.left != fbWidth || R.bottom - R.top != fbHeight)
    {
      // We only resize the window when the canvas is different. This only
      // happens on a manual Resize() from the app, as this is called after
      // the window resizing is finished     
      RECT wndRect;
      LONG style = GetWindowLong (m_hWnd, GWL_STYLE);
      ComputeDefaultRect (wndRect, style);

      // To prevent a second resize of the canvas, we temporarily disable resizing
      AllowResizing = false;

      SetWindowPos (m_hWnd, 0,
    		    wndRect.left, wndRect.top,
		    wndRect.right - wndRect.left, wndRect.bottom - wndRect.top,
		    SWP_NOZORDER);
      
      // Reset. AllowResizing must be true in order to reach this point anyway
      AllowResizing = true;
    }
  }
  return true;
}

void csGraphics2DOpenGL::SetFullScreen (bool b)
{
  if (FullScreen == b) return;
  FullScreen = b;

  if (is_open)
  {
    // Now actually change the window/display settings
    DWORD style;
    if (FullScreen)
    {
      windowModeStyle = GetWindowLong (m_hWnd, GWL_STYLE);
      SwitchDisplayMode (false);
      style = WS_POPUP | WS_VISIBLE | WS_SYSMENU;
      SetWindowLong (m_hWnd, GWL_STYLE, style);
      SetWindowPos (m_hWnd, CS_WINDOW_Z_ORDER, 0, 0, fbWidth, fbHeight, SWP_FRAMECHANGED);
      ShowWindow (m_hWnd, SW_SHOW);
    }
    else
    {
      SwitchDisplayMode (true);
      style = WS_MINIMIZEBOX | WS_POPUP | WS_SYSMENU;
      style |= (windowModeStyle & (WS_CAPTION));
      RECT wndRect;
      ComputeDefaultRect (wndRect, style);
      if (AllowResizing)
        style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
      SetWindowLong (m_hWnd, GWL_STYLE, style);
      SetWindowPos (m_hWnd, HWND_NOTOPMOST,
		    wndRect.left, wndRect.right,
		    wndRect.right - wndRect.left, wndRect.bottom - wndRect.top,
		    SWP_FRAMECHANGED);
      ShowWindow (m_hWnd, SW_SHOW);
    }
  }
}

LRESULT CALLBACK csGraphics2DOpenGL::WindowProc (HWND hWnd, UINT message,
  WPARAM wParam, LPARAM lParam)
{
  csGraphics2DOpenGL *This = (csGraphics2DOpenGL *)GetWindowLongPtrA (hWnd, GWLP_USERDATA);
  switch (message)
  {
    case WM_ACTIVATE:
      {
	This->Activate (!(wParam == WA_INACTIVE));
	break;
      }
    case WM_SIZE:
      {
	/*
	 * If the resizing flag is SIZE_MINIMIZED, then we must not change the Height
	 * or Width of this canvas!
	 * So in the SIZE_MINIMZED case the call to Resize must be avoided.
	 * Besides we must let the old window procedure be called,
	 * which handles the WM_SIZE message as well.
	 * - Luca (groton@gmx.net)
	 */
        if (wParam != SIZE_MINIMIZED)
        {
	  RECT R;
	  GetClientRect (hWnd, &R);
	  This->Resize (R.right - R.left, R.bottom - R.top);
        }
      }
      break;
    case WM_DESTROY:
      This->m_hWnd = 0;
      break;
    case WM_DWMCOMPOSITIONCHANGED:
      /* Desktop compositing was enabled or disabled.
         Try to restore the last set window transparency state. */
      This->csGraphics2DOpenGL::SetWindowTransparent (This->transparencyState);
      break;
  }
  if (IsWindowUnicode (hWnd))
  {
    return CallWindowProcW ((WNDPROC)This->m_OldWndProc, hWnd, message, wParam, lParam);
  }
  else
  {
    return CallWindowProcA ((WNDPROC)This->m_OldWndProc, hWnd, message, wParam, lParam);
  }
}

void csGraphics2DOpenGL::Activate (bool activated)
{
  if (FullScreen && (activated != m_bActivated))
  {
    m_bActivated = activated;
    if (m_bActivated)
    {
      SwitchDisplayMode (false);
      ShowWindow (m_hWnd, SW_RESTORE);
      SetWindowPos (m_hWnd, CS_WINDOW_Z_ORDER, 0, 0, fbWidth, fbHeight, 0);
      wglMakeCurrent (hDC, hGLRC);
    }
    else
    {
      wglMakeCurrent (0, 0);
      SetWindowPos (m_hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE |
        SWP_NOSIZE | SWP_NOACTIVATE);
      ShowWindow (m_hWnd, SW_MINIMIZE);
      SwitchDisplayMode (true);
    }
  }
}

void csGraphics2DOpenGL::SwitchDisplayMode (bool userMode)
{
  DEVMODE curdmode, dmode;

  if (userMode)
  {
    // set the default display mode
    if (modeSwitched)
    {
      ZeroMemory (&dmode, sizeof(dmode));
      curdmode.dmSize = sizeof (dmode);
      curdmode.dmDriverExtra = 0;
      EnumDisplaySettings (0, ENUM_REGISTRY_SETTINGS, &dmode);
      // just do something when the mode was actually switched.
      ChangeDisplaySettings (&dmode, CDS_RESET);
      modeSwitched = false;
    }
  }
  else
  {
    modeSwitched = false;
    // set the user-requested display mode
    ZeroMemory (&curdmode, sizeof(curdmode));
    curdmode.dmSize = sizeof (curdmode);
    curdmode.dmDriverExtra = 0;
    EnumDisplaySettings (0, ENUM_CURRENT_SETTINGS, &curdmode);
    memcpy (&dmode, &curdmode, sizeof (curdmode));

    // check if we already are in the desired display mode
    if (((int)curdmode.dmBitsPerPel == Depth) &&
      ((int)curdmode.dmPelsWidth == fbWidth) &&
      ((int)curdmode.dmPelsHeight == fbHeight) &&
      (!m_nDisplayFrequency || (dmode.dmDisplayFrequency == m_nDisplayFrequency)))
    {
      // no action necessary
      return;
    }
    dmode.dmBitsPerPel = Depth;
    dmode.dmPelsWidth = fbWidth;
    dmode.dmPelsHeight = fbHeight;
    if (m_nDisplayFrequency) dmode.dmDisplayFrequency = m_nDisplayFrequency;
    dmode.dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    
    LONG ti;
    if ((ti = ChangeDisplaySettings(&dmode, CDS_FULLSCREEN)) != DISP_CHANGE_SUCCESSFUL)
    {
      // maybe just the monitor frequency is not supported.
      // so try without setting it.
      // but first check resolution/depth w/o refresh rate
      if (((int)curdmode.dmBitsPerPel == Depth) &&
        ((int)curdmode.dmPelsWidth == fbWidth) &&
        ((int)curdmode.dmPelsHeight == fbHeight))
      {
	refreshRate = curdmode.dmDisplayFrequency;
        // no action necessary
        return;
      }
      dmode.dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT;
      ti = ChangeDisplaySettings(&dmode, CDS_FULLSCREEN);
    }
    if (ti != DISP_CHANGE_SUCCESSFUL)
    {
      //The cases below need error handling, as they are errors.
      switch (ti)
      {
        case DISP_CHANGE_RESTART:
          //computer must restart for mode to work.
          Report (CS_REPORTER_SEVERITY_WARNING,
            "gl2d error: must restart for display change.");
          break;
        case DISP_CHANGE_BADFLAGS:
          //Bad Flag settings
          Report (CS_REPORTER_SEVERITY_WARNING,
            "gl2d error: display change bad flags.");
          break;
        case DISP_CHANGE_FAILED:
          //Failure to display
          Report (CS_REPORTER_SEVERITY_WARNING,
            "gl2d error: display change failed.");
          break;
        case DISP_CHANGE_NOTUPDATED:
          //No Reg Write Error
          Report (CS_REPORTER_SEVERITY_WARNING,
            "gl2d error: display change could not write registry.");
          break;
        default:
          //Unknown Error
          Report (CS_REPORTER_SEVERITY_WARNING,
            "gl2d error: display change gave unknown error.");
          break;
      }
    }
    else
    {
      modeSwitched = true;
    }
  }

  // retrieve actual refresh rate
  ZeroMemory (&curdmode, sizeof(curdmode));
  curdmode.dmSize = sizeof (curdmode);
  curdmode.dmDriverExtra = 0;
  EnumDisplaySettings (0, ENUM_CURRENT_SETTINGS, &curdmode);
  refreshRate = curdmode.dmDisplayFrequency;
}

static BOOL CALLBACK GrabPrimaryMonitor (HMONITOR monitor, HDC, LPRECT, LPARAM data)
{
  *(reinterpret_cast<HMONITOR*> (data)) = monitor;
  return false;
}

csRect csGraphics2DOpenGL::GetWorkspaceRect ()
{
  // Monitor used to get the workspace.
  HMONITOR workspaceMonitor;
  if (!m_hWnd)
  {
    // No window yet: use primary monitor
    if (!primaryMonitor)
    {
      EnumDisplayMonitors (NULL, NULL, &GrabPrimaryMonitor,
                           reinterpret_cast<LPARAM> (&primaryMonitor));
    }
    workspaceMonitor = primaryMonitor;
  }
  else
  {
    workspaceMonitor = MonitorFromWindow (m_hWnd, MONITOR_DEFAULTTOPRIMARY);
  }
  MONITORINFO info;
  memset (&info, 0, sizeof (info));
  info.cbSize = sizeof (info);
  if (!GetMonitorInfo (workspaceMonitor, &info))
  {
    return csRect (0, 0, GetSystemMetrics (SM_CXSCREEN),
                   GetSystemMetrics (SM_CXSCREEN));
  }
  return csRect (info.rcWork.left, info.rcWork.top,
                 info.rcWork.right, info.rcWork.bottom);
}

void csGraphics2DOpenGL::ComputeDefaultRect (RECT& windowRect, LONG style, LONG exStyle)
{
  csRect workspace (GetWorkspaceRect ());
  RECT clientRect;
  clientRect.left = 0;
  clientRect.top = 0;
  clientRect.right = fbWidth;
  clientRect.bottom = fbHeight;
  AdjustWindowRectEx (&clientRect, style, false, exStyle);
  int wndW (clientRect.right - clientRect.left), wndH (clientRect.bottom - clientRect.top);
  windowRect.left = workspace.xmin + (workspace.Width() - wndW) / 2;
  windowRect.top = workspace.ymin + (workspace.Height() - wndH) / 2;
  windowRect.right = windowRect.left + wndW;
  windowRect.bottom = windowRect.top + wndH;
}

bool csGraphics2DOpenGL::GetWorkspaceDimensions (int& width, int& height)
{
  csRect workspace (GetWorkspaceRect ());
  width = workspace.Width();
  height = workspace.Height();
  return true;
}

bool csGraphics2DOpenGL::AddWindowFrameDimensions (int& width, int& height)
{
  RECT clientRect;
  clientRect.left = 0;
  clientRect.top = 0;
  clientRect.right = width;
  clientRect.bottom = height;
  DWORD style (WS_POPUP), exStyle (0);
  if (!FullScreen)
  {
    style |= WS_CAPTION;
    if (AllowResizing) style |= WS_THICKFRAME;
  }
  AdjustWindowRectEx (&clientRect, style, false, exStyle);
  width = clientRect.right - clientRect.left;
  height = clientRect.bottom - clientRect.top;
  return true;
}

bool csGraphics2DOpenGL::IsWindowTransparencyAvailable()
{
  if (!Dwmapi::DwmapiAvailable ()) return false;
  BOOL compositionFlag;
  HRESULT hr = Dwmapi::DwmIsCompositionEnabled (&compositionFlag);
  if (!SUCCEEDED (hr))
    return false;
  return compositionFlag != FALSE; // NB: != avoids warning on MSVC
}

bool csGraphics2DOpenGL::SetWindowTransparent (bool transparent)
{
  if (!csGraphics2DOpenGL::IsWindowTransparencyAvailable()) return false;

  if (!is_open)
  {
    transparencyRequested = transparent;
    return true;
  }

  DWM_BLURBEHIND bb;
  memset (&bb, 0, sizeof (bb));
  bb.dwFlags = DWM_BB_ENABLE;
  bb.fEnable = transparent;
  HRESULT hr = Dwmapi::DwmEnableBlurBehindWindow (m_hWnd, &bb);
  if (SUCCEEDED (hr))
  {
    transparencyState = transparent;
    if (hideDecoClientFrame)
    {
      dwmMARGINS frameMargins = {-1};
      Dwmapi::DwmExtendFrameIntoClientArea (m_hWnd, &frameMargins);
    }
    return true;
  }
  else
    return false;
}

bool csGraphics2DOpenGL::GetWindowTransparent ()
{
  if (!Dwmapi::DwmapiAvailable ()) return false;

  if (!is_open) return transparencyRequested;

  return IsWindowTransparencyAvailable() && transparencyState;
}

bool csGraphics2DOpenGL::SetWindowDecoration (WindowDecoration decoration, bool flag)
{
  if (FullScreen) return false;

  LONG style = GetWindowLong (m_hWnd, GWL_STYLE);
  LONG exstyle = GetWindowLong (m_hWnd, GWL_EXSTYLE);
  RECT newRect;
  GetClientRect (m_hWnd, &newRect);
  ClientToScreen (m_hWnd, (POINT*)(&newRect.left));
  ClientToScreen (m_hWnd, (POINT*)(&newRect.right));
  switch (decoration)
  {
  case decoCaption:
    if (flag)
    {
      style |= WS_CAPTION;
    }
    else
    {
      style &= ~WS_CAPTION;
    }
    break;
  case decoClientFrame:
    {
      if (GetWindowTransparent ())
      {
	dwmMARGINS frameMargins;
	hideDecoClientFrame = !flag;
	if (hideDecoClientFrame)
	{
	  frameMargins.cxLeftWidth = frameMargins.cxRightWidth =
	    frameMargins.cyTopHeight = frameMargins.cyBottomHeight = -1;
	}
	else
	{
	  frameMargins.cxLeftWidth = frameMargins.cxRightWidth =
	    frameMargins.cyTopHeight = frameMargins.cyBottomHeight = 0;
	}
	HRESULT hr = Dwmapi::DwmExtendFrameIntoClientArea (m_hWnd, &frameMargins);
	return SUCCEEDED (hr);
      }
    }
    // else fall through
  default:
    return false;
  }

  SetWindowLong (m_hWnd, GWL_STYLE, style);
  AdjustWindowRectEx (&newRect, style, false, exstyle);
  SetWindowPos (m_hWnd, 0,
		newRect.left, newRect.top,
		newRect.right - newRect.left, newRect.bottom - newRect.top,
		SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOZORDER);
  return true;
}

bool csGraphics2DOpenGL::GetWindowDecoration (WindowDecoration decoration)
{
  LONG style = GetWindowLong (m_hWnd, GWL_STYLE);
  switch (decoration)
  {
  case decoCaption:
    return (style & WS_CAPTION) != 0;
  case decoClientFrame:
    {
      // Client frame not drawn w/o compositing
      if (!IsWindowTransparencyAvailable()) return false;
      return !hideDecoClientFrame;
    }
    break;
  default:
    break;
  }

  return csGraphics2DGLCommon::GetWindowDecoration (decoration);
}
