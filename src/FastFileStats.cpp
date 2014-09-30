///////////////////////////////////////////////////////////////////////////////////////////////////
// Server main entry point.

#include "stdafx.h"
#include "FastFileStats.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

HINSTANCE ThisModule() {
  return reinterpret_cast<HINSTANCE>(&__ImageBase);
}

template <typename T, typename U>
T VerifyNot(T actual, U error) {
  if (actual != error)
    return actual;

  volatile ULONG err = ::GetLastError();
  __debugbreak();
  __assume(0);
}

typedef LRESULT (* MsgCallBack)(HWND, WPARAM, LPARAM);
struct MessageHandler {
  UINT message;
  MsgCallBack callback;
};


HWND MakeWindow(
    const wchar_t* title, ULONG style, HMENU menu, const SIZE& size, MessageHandler* handlers) {
  WNDCLASSEXW wcex = {sizeof(wcex)};
  wcex.hCursor = ::LoadCursorW(NULL, IDC_ARROW);
  wcex.hInstance = ThisModule();
  wcex.lpszClassName = __FILEW__;
  wcex.lpfnWndProc = [] (HWND window, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT {
    static MessageHandler* s_handlers = reinterpret_cast<MessageHandler*>(lparam);
    size_t ix = 0;
    while (s_handlers[ix].message != -1) {
      if (s_handlers[ix].message == message)
        return s_handlers[ix].callback(window, wparam, lparam);
      ++ix;
    }

    return ::DefWindowProcW(window, message, wparam, lparam);
  };

  wcex.lpfnWndProc(NULL, 0, 0, reinterpret_cast<UINT_PTR>(handlers));
  ATOM atom = VerifyNot(::RegisterClassExW(&wcex), 0);
  int pos_def = CW_USEDEFAULT;
  return ::CreateWindowExW(0, MAKEINTATOM(atom), title, style,
                           pos_def, pos_def, size.cx, size.cy,
                           NULL, menu, ThisModule(), NULL); 
}

int RunMainUILoop() {
  static HPEN pen_dot = ::CreatePen(PS_DOT, 1, RGB(0, 0, 0));

  MessageHandler msg_handlers[] = {
    { WM_PAINT, [] (HWND window, WPARAM, LPARAM) -> LRESULT {
      PAINTSTRUCT ps;
      HDC dc = ::BeginPaint(window, &ps);
      ::Rectangle(dc, 10, 10, 100, 100);
      ::EndPaint(window, &ps);
      return 0;
    }},

    { WM_ERASEBKGND, [] (HWND window, WPARAM wparam, LPARAM) -> LRESULT {
      RECT rect;
      VerifyNot(::GetClientRect(window, &rect), FALSE);
      HGDIOBJ prev_obj = ::SelectObject(HDC(wparam), pen_dot);
      ::Rectangle(HDC(wparam), rect.left, rect.top, rect.right, rect.bottom);
      ::SelectObject(HDC(wparam),  prev_obj);
      return 0;
    }},

    { WM_DISPLAYCHANGE, [] (HWND window, WPARAM, LPARAM) -> LRESULT {
      ::InvalidateRect(window, NULL, TRUE);
      return 0;
    }},

    { WM_CLOSE, [] (HWND window, WPARAM, LPARAM) -> LRESULT {
      ::PostQuitMessage(0);
      return 0;
    }},

    { WM_ENDSESSION, [] (HWND window, WPARAM, LPARAM) -> LRESULT {
      ::PostQuitMessage(0);
      return 0;
    }},

    {-1, NULL}
  };

  SIZE size = {200, 200};
  HWND main_window = VerifyNot(
      MakeWindow(L"fastfilestats/by/cpu",
                 WS_OVERLAPPEDWINDOW | WS_VISIBLE, NULL,
                 size,
                 msg_handlers), HWND(NULL));

  MSG msg;
  while (VerifyNot(::GetMessageW(&msg, NULL, 0, 0), -1)) {
    ::DispatchMessageW(&msg);
  }

  return 0;
}

int __stdcall wWinMain(HINSTANCE module, HINSTANCE, wchar_t* cc, int) {

  return RunMainUILoop();
}
