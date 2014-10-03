///////////////////////////////////////////////////////////////////////////////////////////////////
// Server main entry point.

#include "stdafx.h"

#include <string>
#include <tuple>
#include <vector>

#include "resource.h"
#include "FastFileStats.h"

#define PARANOID 1

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

bool AddDir(const wchar_t* name) {
  if (name[0] != '.')
    return true;
  if (name[1] == 0)
    return false;
  if (name[1] != '.')
    return true;
  return false;
}

bool CreateFFS(BYTE* const start, DWORD size, const wchar_t* top_dir) {
  auto mem = start;
  auto header = reinterpret_cast<FFS_Header*>(mem);
  *header = FFS_Header{FFS_kMagic, FFS_kVersion, FFS_kBooting, 0, 0, 0};
  mem += sizeof(*header);
  auto w32fd = reinterpret_cast<WIN32_FIND_DATA*>(mem);

  typedef std::tuple<std::wstring, DWORD> Entry;

  std::vector<Entry> pending_dirs;
  std::vector<Entry> found_dirs;

  DWORD all_count = 0;
  DWORD dir_count = 0;
  DWORD pending_fixes = 0;
  DWORD reparse_count = 0;

  pending_dirs.emplace_back(top_dir, 0);

  while (pending_dirs.size()) {
    for (auto& e : pending_dirs) {
      auto wildc = std::get<0>(e) + L"\\*";
      auto fff = ::FindFirstFileW(wildc.c_str(), w32fd);
      if (fff == INVALID_HANDLE_VALUE) {
        ++pending_fixes;
        continue;
      }

      w32fd->dwReserved0 = std::get<1>(e);
      ++all_count;

#if defined(PARANOID)
      // The first entry is always ".".
      if ((w32fd->cFileName[0] != '.') || (w32fd->cFileName[1] != 0))
        __debugbreak();
#endif

      ++w32fd;
      while (::FindNextFileW(fff, w32fd)) {
        // stuff the offset to the parent directory.
        w32fd->dwReserved0 = std::get<1>(e);

        if (w32fd->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
          ++reparse_count;
        } else if (w32fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
          if (AddDir(w32fd->cFileName)) {
            found_dirs.emplace_back((std::get<0>(e) + L"\\") + w32fd->cFileName, 
                                     DWORD(w32fd) - DWORD(start));
            ++dir_count;
          }
        }

        ++all_count;
        ++w32fd;
      }

      ::FindClose(fff);
    }

    pending_dirs.swap(found_dirs);
    found_dirs.clear();
  }


  header->bytes = DWORD(w32fd) - DWORD(start);
  header->num_dirs = dir_count;
  header->num_nodes = all_count;
  header->status = FFS_kUpdating;

  return true;
}


int ExceptionFilter(EXCEPTION_POINTERS *ep, BYTE* start, DWORD max_size) {
  if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
    return EXCEPTION_CONTINUE_SEARCH;
  auto addr = reinterpret_cast<BYTE*>(ep->ExceptionRecord->ExceptionInformation[1]);
  if ((addr < start) || (addr > (start + max_size)))
    return EXCEPTION_CONTINUE_SEARCH;
  // In our range, map another meg.
  auto new_addr = ::VirtualAlloc(addr, 1024 * 1024, MEM_COMMIT, PAGE_READWRITE);
  if (!new_addr)
    EXCEPTION_EXECUTE_HANDLER;
  return EXCEPTION_CONTINUE_EXECUTION;
}

int __stdcall wWinMain(HINSTANCE module, HINSTANCE, wchar_t* cc, int) {
  const wchar_t dir[] = L"f:\\src";

  // All data must fit in 500MB.
  const DWORD kMaxSize = 1024 * 1024 * 500;
  auto mmap = ::CreateFileMappingW(
      INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_RESERVE, 0, kMaxSize, L"ffs_dir01");
  auto start = reinterpret_cast<BYTE*>(
      ::MapViewOfFile(mmap, FILE_MAP_ALL_ACCESS, 0, 0, kMaxSize));
  if (!start)
    return 1;

  __try {
    if (!CreateFFS(start, kMaxSize, dir))
      return 2;

  } __except (ExceptionFilter(GetExceptionInformation(), start, kMaxSize)) {
    // Probably ran out of memory.
    return 3;
  }

  return RunMainUILoop();
}
