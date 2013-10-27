/*
* BearLibTerminal
* Copyright (C) 2013 Cfyz
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
* of the Software, and to permit persons to whom the Software is furnished to do
* so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
* FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
* COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
* IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
* CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#ifdef _WIN32

#include <map>
#include <future>
#include "WinApiWindow.hpp"
#include "BearLibTerminal.h"
#include "Point.hpp"
#include "Log.hpp"

#include <Mmsystem.h>

#define WM_CUSTOM_SETSIZE (WM_APP+1)
#define WM_CUSTOM_SETTITLE (WM_APP+2)
#define WM_CUSTOM_SETICON (WM_APP+3)
#define WM_CUSTOM_INVOKE (WM_APP+4)
#define WM_CUSTOM_INVOKE2 (WM_APP+5)

namespace BearLibTerminal
{
	std::wstring GetLastErrorStr(std::uint32_t rc)
	{
		const size_t buffer_size = 256;
		wchar_t buffer[buffer_size];
		DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM, 0, rc, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), buffer, buffer_size, NULL);
		std::wstring result(buffer, n);
		if ( result.find(L"\r\n") != std::wstring::npos ) result = result.substr(0, result.length()-2);
		if ( result.length() > 0 && result.back() == '.' ) result = result.substr(0, result.length()-1);
		return result;
	}

	std::wstring GetLastErrorStr()
	{
		return GetLastErrorStr(GetLastError());
	}

	uint64_t StartTiming()
	{
		LARGE_INTEGER t;
		QueryPerformanceCounter(&t);
		return t.QuadPart;
	}

	uint64_t StopTiming(uint64_t start)
	{
		LARGE_INTEGER t, f;
		QueryPerformanceCounter(&t);
		QueryPerformanceFrequency(&f);
		return ((t.QuadPart-start)/(double)f.QuadPart)*1000000;
	}

	WinApiWindow::WinApiWindow():
		m_class_name(L"BearLibTerminal"),
		m_handle(nullptr),
		m_device_context(nullptr),
		m_rendering_context(nullptr)
	{ }

	WinApiWindow::~WinApiWindow()
	{
		Stop();
	}

	static HICON LoadIconFromFile(const std::wstring& filename, bool small)
	{
		HICON result = (HICON)LoadImageW
		(
			NULL,
			filename.c_str(),
			IMAGE_ICON,
			GetSystemMetrics(small? SM_CXSMICON: SM_CXICON),
			GetSystemMetrics(small? SM_CYSMICON: SM_CYICON),
			LR_LOADFROMFILE
		);

		if ( result == NULL )
		{
			LOG(Warning, L"Failed to load " << (small? L"small": L"large") << " icon from file (" << GetLastErrorStr() << ")" );
		}

		return result;
	}

	bool WinApiWindow::ValidateIcon(const std::wstring& filename)
	{
		// TODO
		return true;
	}

	void WinApiWindow::SetTitle(const std::wstring& title)
	{
		std::lock_guard<std::mutex> guard(m_lock);
		if ( m_handle ) PostMessage(m_handle, WM_CUSTOM_SETTITLE, (WPARAM)NULL, (LPARAM)new std::wstring(title));
	}

	void WinApiWindow::SetIcon(const std::wstring& filename)
	{
		if ( filename.empty() ) return;
		std::lock_guard<std::mutex> guard(m_lock);
		if ( m_handle ) PostMessage(m_handle, WM_CUSTOM_SETICON, (WPARAM)NULL, (LPARAM)new std::wstring(filename));
	}

	void WinApiWindow::SetClientSize(const Size& size)
	{
		std::lock_guard<std::mutex> guard(m_lock);
		if ( m_handle ) PostMessage(m_handle, WM_CUSTOM_SETSIZE, (WPARAM)NULL, (LPARAM)new Size(size));
	}

	void WinApiWindow::Redraw()
	{
		std::lock_guard<std::mutex> guard(m_lock);
		if ( m_handle == nullptr ) return;
		InvalidateRect(m_handle, NULL, FALSE);
		m_redraw_barrier.Wait();
	}

	void WinApiWindow::Show()
	{
		std::lock_guard<std::mutex> guard(m_lock);
		if ( m_handle != nullptr )
		{
			ShowWindow(m_handle, SW_SHOW);
			SetForegroundWindow(m_handle);
		}
	}

	void WinApiWindow::Hide()
	{
		std::lock_guard<std::mutex> guard(m_lock);
		if ( m_handle != nullptr ) ShowWindow(m_handle, SW_HIDE);
	}

	struct InvokationSentry
	{
		std::function<void()> func;
		Semaphore semaphore;
	};

	struct InvokationSentry2
	{
		std::packaged_task<void()> task;

		InvokationSentry2(std::function<void()> func):
			task(func)
		{ }
	};

	void WinApiWindow::Invoke(std::function<void()> func)
	{
		if (m_handle)
		{
			/*
			std::shared_ptr<InvokationSentry> sentry = std::make_shared<InvokationSentry>();
			sentry->func = func;
			PostMessage(m_handle, WM_CUSTOM_INVOKE, (WPARAM)NULL, (LPARAM)&sentry);
			sentry->semaphore.Wait();
			/*/
			auto sentry = std::make_shared<InvokationSentry2>(func);
			std::future<void> future = sentry->task.get_future();
			PostMessage(m_handle, WM_CUSTOM_INVOKE2, (WPARAM)NULL, (LPARAM)&sentry);
			future.get();
			//*/
		}
	}

	void WinApiWindow::ThreadFunction()
	{
		MSG msg;

		while( m_proceed )
		{
			if ( PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE) )
			{
				if ( msg.message == WM_QUIT ) break;

				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}
			else
			{
				Sleep(10);
			}
		}
	}

	bool WinApiWindow::Construct()
	{
		std::lock_guard<std::mutex> guard(m_lock);

		if ( !CreateWindowObject() || !CreateOpenGLContext() )
		{
			DestroyUnlocked();
			return false;
		}

		m_proceed = true;
		return true;
	}

	void WinApiWindow::Destroy()
	{
		std::lock_guard<std::mutex> guard(m_lock);
		DestroyUnlocked();
	}

	void WinApiWindow::DestroyUnlocked()
	{
		DestroyOpenGLContext();
		DestroyWindowObject();
	}

	bool WinApiWindow::CreateWindowObject()
	{
		Size size(640, 480);
		std::wstring m_title(L"BearLibTerminal");

		HINSTANCE app_instance = GetModuleHandle(NULL);

		WNDCLASSW wc;
		memset(&wc, 0, sizeof(wc));
		wc.style			= CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
		wc.lpfnWndProc		= (WNDPROC)SharedWindowProc;
		wc.hInstance		= app_instance;
		wc.hIcon			= LoadIcon(NULL, IDI_WINLOGO);
		wc.hCursor			= LoadCursor(NULL, IDC_ARROW);
		wc.lpszClassName	= m_class_name.c_str();

		if ( !RegisterClassW(&wc) )
		{
			LOG(Fatal, L"Failed to register window class (" << GetLastErrorStr() << ")");
			return false; // Nothing to destroy yet
		}

		DWORD style =
			WS_BORDER|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|
			WS_CLIPSIBLINGS|WS_CLIPCHILDREN;

		RECT rect = { 0, 0, size.width, size.height };
		AdjustWindowRect(&rect, style, FALSE);
		int width = rect.right-rect.left;
		int height = rect.bottom-rect.top;

		m_handle = CreateWindowW
			(
				m_class_name.c_str(),
				m_title.c_str(),
				style,
				CW_USEDEFAULT, CW_USEDEFAULT,
				width, height,
				NULL, NULL,
				app_instance,
				NULL
			);

		if ( m_handle == nullptr )
		{
			LOG(Fatal, L"Failed to create window (" << GetLastErrorStr() << ")");
			DestroyWindowObject();
			return false;
		}

		// Hook custom WndProc
		SetWindowLongPtrW(m_handle, GWL_USERDATA, (LONG_PTR)(void*)this);

		return true;
	}

	bool WinApiWindow::CreateOpenGLContext()
	{
		m_device_context = GetDC(m_handle);
		if ( m_device_context == nullptr )
		{
			LOG(Fatal, L"Failed to retrieve device context (" << GetLastErrorStr() << ")");
			return false; // Nothing to destroy yet
		}

		PIXELFORMATDESCRIPTOR pfd;
		memset(&pfd, sizeof(pfd), 0);
		pfd.nSize = sizeof(pfd);
		pfd.nVersion = 1;
		pfd.dwFlags = PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER;
		pfd.iPixelType = PFD_TYPE_RGBA;
		pfd.cColorBits = 24; // At least, driver may choose PFD with higher color depth
		pfd.iLayerType = PFD_MAIN_PLANE;

		int pixel_format = ChoosePixelFormat(m_device_context, &pfd);
		if ( pixel_format == 0 )
		{
			LOG(Fatal, L"Failed to choose suitable pixel format (" << GetLastErrorStr() << ")");
			DestroyOpenGLContext();
			return false;
		}

		if ( SetPixelFormat(m_device_context, pixel_format, &pfd) == 0 )
		{
			LOG(Fatal, L"Failed to apply chosen pixel format (" << GetLastErrorStr() << ")");
			DestroyOpenGLContext();
			return false;
		}

		m_rendering_context = wglCreateContext(m_device_context);
		if ( m_rendering_context == nullptr )
		{
			LOG(Fatal, L"Failed to create OpenGL context (" << GetLastErrorStr() << ")");
			DestroyOpenGLContext();
			return false;
		}

		/*
		if ( wglMakeCurrent(m_device_context, m_rendering_context) == 0 )
		{
			LOG(Fatal, L"Failed to bind OpenGL context (" << GetLastErrorStr() << ")");
			DestroyOpenGLContext();
			return false;
		}
		*/
		if (!AcquireRC())
		{
			LOG(Fatal, L"Failed to acquire rendering context");
			DestroyOpenGLContext();
			return false;
		}

		// FIXME: check availability, clean up the code
		typedef BOOL (*PFNWGLSWAPINTERVALEXT)(int);
		PFNWGLSWAPINTERVALEXT f = (PFNWGLSWAPINTERVALEXT)wglGetProcAddress("wglSwapIntervalEXT");
		if ( f ) f(0);
		else LOG(Trace, "No wglSwapIntervalEXT?");

		return true;
	}

	bool WinApiWindow::AcquireRC()
	{
		if (wglMakeCurrent(m_device_context, m_rendering_context) == 0)
		{
			LOG(Error, L"Failed to bind OpenGL context (" << GetLastErrorStr() << ")");
			return false;
		}

		return true;
	}

	bool WinApiWindow::ReleaseRC()
	{
		if (wglMakeCurrent(NULL, NULL) == 0)
		{
			LOG(Error, L"Failed to unbind OpenGL context (" << GetLastErrorStr() << ")");
			return false;
		}

		return true;
	}

	void WinApiWindow::DestroyWindowObject()
	{
		if ( m_handle )
		{
			DestroyWindow(m_handle);
			m_handle = nullptr;
		}

		HINSTANCE app_instance = GetModuleHandleW(NULL);
		UnregisterClassW(m_class_name.c_str(), app_instance);
	}

	void WinApiWindow::DestroyOpenGLContext()
	{
		if ( m_rendering_context != nullptr )
		{
			//wglMakeCurrent(NULL, NULL);
			ReleaseRC();
			m_rendering_context = nullptr;
		}

		if ( m_device_context )
		{
			ReleaseDC(m_handle, m_device_context);
			m_device_context = nullptr;
		}
	}

	LRESULT CALLBACK WinApiWindow::SharedWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
	{
		WinApiWindow* p = (WinApiWindow*)GetWindowLongPtrW(hwnd, GWL_USERDATA);

		return (p == nullptr)?
			DefWindowProc(hwnd, uMsg, wParam, lParam):
			p->WindowProc(uMsg, wParam, lParam);
	}

	uint64_t tt_val = 0;
	uint64_t tt_num = 0;
	uint64_t tt_las = 0;

	LRESULT WinApiWindow::HandleWmPaint(WPARAM wParam, LPARAM lParam)
	{
		try
		{
			//*
			if ( m_on_redraw ) m_on_redraw();
			SwapBuffers(m_device_context);
			/*/
			uint64_t s = StartTiming();
			if ( m_on_redraw ) m_on_redraw();
			SwapBuffers(m_device_context);
			uint64_t d = StopTiming(s);
			tt_val += d;
			tt_num += 1;
			uint64_t las = timeGetTime();
			if (las > tt_las + 1000)
			{
				float tt_avg = tt_val / (float)tt_num;
				float tt_exp = 1000000.0f / tt_avg;
				tt_val = 0;
				tt_num = 0;
				tt_las = las;

				std::wostringstream ss;
				ss << "average frame: " << tt_avg << " us, potential fps: " << tt_exp;
				SetWindowTextW(m_handle, ss.str().c_str());
			}
			//*/
		}
		catch ( std::exception& e )
		{
			LOG(Fatal, L"Rendering routine has thrown an exception: " << e.what());
			m_proceed = false;
		}

		// Mark window area as processed
		RECT rect;
		GetClientRect(m_handle, &rect); // TODO: May be cached
		ValidateRect(m_handle, &rect);

		// Open barrier
		m_redraw_barrier.Notify();

		return FALSE;
	}

	static uint8_t MapNavigationScancodeToNumpad(uint8_t scancode)
	{
		typedef std::map<uint8_t, uint8_t> mapping_t;
		static mapping_t mapping = []() -> mapping_t
		{
			mapping_t result;
			result[VK_INSERT]	= VK_NUMPAD0;
			result[VK_DELETE]	= VK_DECIMAL;
			result[VK_END]		= VK_NUMPAD1;
			result[VK_DOWN]		= VK_NUMPAD2;
			result[VK_NEXT]		= VK_NUMPAD3;
			result[VK_LEFT]		= VK_NUMPAD4;
			result[VK_CLEAR]	= VK_NUMPAD5;
			result[VK_RIGHT]	= VK_NUMPAD6;
			result[VK_HOME]		= VK_NUMPAD7;
			result[VK_UP]		= VK_NUMPAD8;
			result[VK_PRIOR]	= VK_NUMPAD9;
			return result;
		}();

		return mapping[scancode];
	}

	static uint8_t MapOEMScancodeToCharacter(uint8_t scancode)
	{
		typedef std::map<uint8_t, uint8_t> mapping_t;
		static mapping_t mapping = []() -> mapping_t
		{
			mapping_t result;
			result[VK_OEM_1]		= ';';
			result[VK_OEM_PLUS]		= '=';
			result[VK_OEM_COMMA]	= ',';
			result[VK_OEM_MINUS]	= '-';
			result[VK_OEM_PERIOD]	= '.';
			result[VK_OEM_2]		= '/';
			result[VK_OEM_3]		= '`';
			result[VK_OEM_4]		= '[';
			result[VK_OEM_5]		= '\\';
			result[VK_OEM_6]		= ']';
			result[VK_OEM_7]		= '\'';
			result[VK_OEM_102]		= '\\';
			return result;
		}();

		return mapping[scancode];
	}

	LRESULT WinApiWindow::WindowProc(UINT uMsg, WPARAM wParam, LPARAM lParam)
	{
		if ( uMsg == WM_CLOSE )
		{
			// Emulate pressing virtual VK_CLOSE button
			ReportInput(Keystroke(VK_CLOSE, false));
			return FALSE;
		}
		else if ( uMsg == WM_PAINT )
		{
			return HandleWmPaint(wParam, lParam);
		}
		else if ( uMsg == WM_ERASEBKGND )
		{
			return TRUE;
		}
		else if ( uMsg == WM_CHAR )
		{
			// This handles all keys with printable characters, as well as
			// * Escape, Tab, Backspace, Enter

			wchar_t charcode = (wchar_t)wParam;
			uint8_t scancode = ((uint32_t)lParam >> 16) & 0xFF;
			bool extended = (lParam & (1<<24)) > 0;
			scancode = MapVirtualKeyW(scancode, MAPVK_VSC_TO_VK);

			if ( scancode == VK_INSERT ||
				 scancode == VK_DELETE ||
				(scancode >= VK_PRIOR && scancode <= VK_DOWN) ||
				(charcode == '/' && extended) )
			{
				// Ignore, handled by WM_KEYxxx
			}
			else if ( scancode == VK_ESCAPE ||
					  scancode == VK_TAB ||
					  scancode == VK_BACK ||
					  scancode == VK_RETURN )
			{
				ReportInput(Keystroke(scancode));
			}
			else
			{
				// Convert OEM scancodes (brackets, commas, etc.)
				if ( scancode == VK_OEM_102 ||
					(scancode >= VK_OEM_1 && scancode <= VK_OEM_3) ||
					(scancode >= VK_OEM_4 && scancode <= VK_OEM_7) )
				{
					//scancode = MapOEMScancodeToCharacter(scancode);
				}

				if ( scancode == VK_OEM_102 ) scancode = VK_OEM_5;

				ReportInput(Keystroke(scancode, charcode, false));
			}

			return FALSE;
		}
		else if ( uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN || uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP )
		{
			// This handles keys not handled by WM_CHAR message
			// * Ctrl, Shift
			// * F1-F12
			// * Break
			// * Navigation: Insert, Home, PgUp, Delete, End, PgDown + Arrows
			// * Numpad

			uint8_t scancode = (uint8_t)(wParam & 0xFF);
			bool extended = (lParam & (1<<24)) > 0;
			bool pressed = (uMsg == WM_KEYDOWN) || (uMsg == WM_SYSKEYDOWN);

			if ( (uMsg == WM_SYSKEYDOWN || uMsg == WM_SYSKEYUP) && scancode != VK_F10 )
			{
				return DefWindowProc(m_handle, uMsg, wParam, lParam);
			}

			if ( scancode == VK_CONTROL ||
				 scancode == VK_SHIFT ||
				 scancode == VK_PAUSE ||
				(scancode >= VK_F1 && scancode <= VK_F12) ||
				(scancode >= VK_NUMPAD0 && scancode <= VK_DIVIDE) )
			{
				ReportInput(Keystroke(scancode, !pressed));
			}
			else if ( scancode == VK_CLEAR ||
					  scancode == VK_INSERT ||
					  scancode == VK_DELETE ||
					 (scancode >= VK_PRIOR && scancode <= VK_DOWN) )
			{
				if ( !extended ) scancode = MapNavigationScancodeToNumpad(scancode);
				ReportInput(Keystroke(scancode, !pressed));
			}
			else if ( !pressed )
			{
				// Handle key release since WM_CHAR does not intercept those
				// OEM must be converted both on press and on release

				wchar_t charcode = 0;

				if ( scancode == VK_OEM_102 ||
					(scancode >= VK_OEM_1 && scancode <= VK_OEM_3) ||
					(scancode >= VK_OEM_4 && scancode <= VK_OEM_7) )
				{
					//scancode = MapOEMScancodeToCharacter(scancode);
					charcode = (wchar_t)MapOEMScancodeToCharacter(scancode);
				}

				if ( scancode == VK_OEM_102 ) scancode = VK_OEM_5;

				ReportInput(Keystroke(scancode, charcode, !pressed));
			}

			return FALSE;
		}
		else if ( uMsg == WM_MOUSEMOVE )
		{
			Point precise_position(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));

			if ( precise_position.x < 0 ) precise_position.x = 0;
			if ( precise_position.y < 0 ) precise_position.y = 0;

			Keystroke stroke;
			stroke.scancode = VK_MOUSE_MOVE;
			stroke.x = precise_position.x;
			stroke.y = precise_position.y;
			ReportInput(stroke);

			return 0;
		}
		else if ( uMsg == WM_MOUSEWHEEL )
		{
			int delta = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
			m_mouse_wheel += delta;

			Keystroke stroke;
			stroke.scancode = VK_MOUSE_SCROLL;
			stroke.z = m_mouse_wheel;
			ReportInput(stroke);

			return 0;
		}
		else if ( uMsg == WM_LBUTTONDOWN || uMsg == WM_LBUTTONUP )
		{
			Keystroke stroke;
			stroke.scancode = VK_LBUTTON;
			stroke.released = uMsg == WM_LBUTTONUP;
			ReportInput(stroke);

			return 0;
		}
		else if ( uMsg == WM_RBUTTONDOWN || uMsg == WM_RBUTTONUP )
		{
			Keystroke stroke;
			stroke.scancode = VK_RBUTTON;
			stroke.released = uMsg == WM_LBUTTONUP;
			ReportInput(stroke);

			return 0;
		}
		else if ( uMsg == WM_MBUTTONDOWN || uMsg == WM_MBUTTONUP )
		{
			Keystroke stroke;
			stroke.scancode = VK_MBUTTON;
			stroke.released = uMsg == WM_LBUTTONUP;
			ReportInput(stroke);

			return 0;
		}
		else if ( uMsg == WM_ACTIVATE )
		{
			if ( wParam == WA_ACTIVE || wParam == WA_CLICKACTIVE )
			{
				if ( m_on_activate ) m_on_activate();
			}
		}
		else if ( uMsg == WM_CUSTOM_SETSIZE )
		{
			auto size = (Size*)lParam;
			DWORD style = GetWindowLongW(m_handle, GWL_STYLE);
			RECT rectangle = { 0, 0, size->width, size->height };
			AdjustWindowRect(&rectangle, style, FALSE);
			BOOL rc = SetWindowPos
			(
				m_handle,
				HWND_NOTOPMOST,
				0, 0,
				rectangle.right-rectangle.left,
				rectangle.bottom-rectangle.top,
				SWP_NOMOVE|SWP_NOREDRAW
			);
			if ( !rc )
			{
				LOG(Error, L"Failed to update window size (" << GetLastErrorStr() << ")");
			}

			delete size;
			return 0;
		}
		else if ( uMsg == WM_CUSTOM_SETTITLE )
		{
			auto title = (std::wstring*)lParam;
			SetWindowTextW(m_handle, title->c_str());
			delete title;
			return 0;
		}
		else if ( uMsg == WM_CUSTOM_SETICON )
		{
			auto filename = reinterpret_cast<std::wstring*>(lParam);

			HICON new_small_icon = LoadIconFromFile(*filename, true);
			if ( new_small_icon != NULL )
			{
				HICON old_small_icon = (HICON)SetClassLong(m_handle, GCL_HICONSM, (LONG)new_small_icon);
				DWORD rc = GetLastError();
				if ( rc == 0 )
				{
					if ( old_small_icon != NULL ) DestroyIcon(old_small_icon);
				}
				else
				{
					LOG(Error, L"Failed to apply small icon (" << GetLastErrorStr() << ")");
				}
			}

			HICON new_large_icon = LoadIconFromFile(*filename, false);
			if ( new_large_icon != NULL )
			{
				HICON old_large_icon = (HICON)SetClassLong(m_handle, GCL_HICON, (LONG)new_large_icon);
				DWORD rc = GetLastError();
				if ( rc == 0 )
				{
					if ( old_large_icon != NULL ) DestroyIcon(old_large_icon);
				}
				else
				{
					LOG(Error, L"Failed to apply large icon (" << GetLastErrorStr() << ")");
				}
			}

			delete filename;
			return 0;
		}
		else if (uMsg == WM_CUSTOM_INVOKE)
		{
			// shared_ptr is necessary because semaphore does not synchronize its own
			// lifetime so caller thread may proceed and deallocate sentry while callee
			// is still executing Notify()
			std::shared_ptr<InvokationSentry> sentry = *(std::shared_ptr<InvokationSentry>*)lParam;
			sentry->func();
			sentry->semaphore.Notify();
			return 0;
		}
		else if (uMsg == WM_CUSTOM_INVOKE2)
		{
			// shared_ptr is necessary because semaphore does not synchronize its own
			// lifetime so caller thread may proceed and deallocate sentry while callee
			// is still executing Notify()
			auto sentry = *(std::shared_ptr<InvokationSentry2>*)lParam;
			sentry->task();
			return 0;
		}

		return DefWindowProc(m_handle, uMsg, wParam, lParam);
	}

	void WinApiWindow::ReportInput(const Keystroke& keystroke)
	{
		if ( m_on_input ) m_on_input(keystroke);
	}
}

#endif