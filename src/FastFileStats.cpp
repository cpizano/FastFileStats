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

bool EndsWith(const std::wstring& full, const std::wstring& ending) {
  if (full.length() < ending.length())
    return false;

  return (0 == full.compare(full.length() - ending.length(), ending.length(), ending));
}

// adapted to start from the back.
DWORD Hash_FNV1a_32(const BYTE* bp, size_t len) {
  auto be = bp + len - 1;
  DWORD hval = 0x811c9dc5UL;
  while (bp <= be) {
    hval ^= (DWORD)*be--;
    hval += (hval << 1) + (hval << 4) + (hval << 7) +
            (hval << 8) + (hval << 24);
  }
  return hval;
}

DWORD FileHash(const std::wstring& fname) {
  return Hash_FNV1a_32(reinterpret_cast<const BYTE*>(&fname[0]), fname.size() * sizeof(wchar_t));
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

  std::vector<DWORD> dir_offsets[FFS_BucketCount];

  DWORD all_count = 0;
  DWORD dir_count = 0;
  DWORD pending_fixes = 0;
  DWORD reparse_count = 0;

  // Create a fake node with the root so code does not have special cases.
  w32fd->dwFileAttributes = -1;
  w32fd->dwReserved0 = 0;
  w32fd->dwReserved1 = 0x8888;
  wcscpy_s(w32fd->cFileName, top_dir);
  pending_dirs.emplace_back(top_dir, DWORD(w32fd) - DWORD(start));
  ++w32fd;

  while (pending_dirs.size()) {
    for (auto& e : pending_dirs) {
      auto wildc = std::get<0>(e) + L"\\*";
      auto fff = ::FindFirstFileW(wildc.c_str(), w32fd);
      if (fff == INVALID_HANDLE_VALUE) {
        ++pending_fixes;
        continue;
      }
      ++all_count;
      // stuff the offset to the parent directory.
      w32fd->dwReserved0 = std::get<1>(e);

      auto hash = FileHash(std::get<0>(e));
      dir_offsets[hash % FFS_BucketCount].emplace_back(DWORD(w32fd) - DWORD(start));
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

#if defined(PARANOID)
  int cc_h = 0;
  int cc_l = 0;
  for (auto& i : dir_offsets) {
    if (i.size() > 67) ++cc_h;
    if (i.size() < 5) ++cc_l;
  }
  if (cc_h > 10)
    __debugbreak();
  if (cc_l > 10)
    __debugbreak();
#endif

  auto next = reinterpret_cast<ULONG_PTR>(w32fd) + 16;
  next &= 0xfffffff0;
  auto next_offset = reinterpret_cast<DWORD*>(next);
  *next_offset = 0xAA55AA55;
  ++next_offset;

  DWORD bucket_offsets[FFS_BucketCount];

  int ix = 0;
  for (auto& dof : dir_offsets) {
    bucket_offsets[ix++] = DWORD(next_offset) - DWORD(start);
    for (auto& dir : dof) {
      *next_offset = dir;
      ++next_offset;
    }
    *next_offset = 0;
    ++next_offset;
  }
  
  auto ffs_dir = reinterpret_cast<FFS_Dir*>(next_offset);
  ffs_dir->count = dir_count;
  memcpy(&ffs_dir->nodes[0], &bucket_offsets[0], FFS_BucketCount * sizeof(DWORD));
  header->dir_offset = DWORD(ffs_dir) - DWORD(start);
  header->status = FFS_kFinished;
  return true;
}

bool MatchesDirChain(DWORD start, const WIN32_FIND_DATA* w32fd, const std::wstring& path) {
  std::wstring term(w32fd->cFileName);
  if (!EndsWith(path, term))
    return false;
  if (!w32fd->dwReserved0) {
    // reached the root of our data. this is the fake node that contains the absolute path
    // the the root of the enumeration.
    return (path == w32fd->cFileName);
  }
  // recurse.
  auto remains = std::wstring(path, 0, path.size() - term.size() - 1);
  auto nfd = reinterpret_cast<WIN32_FIND_DATA*>(w32fd->dwReserved0 + start);
  return MatchesDirChain(start, nfd, remains);
}

const WIN32_FIND_DATA* GetDirectory(const FFS_Header* header, const std::wstring& path) {
  if (path.empty())
    return nullptr;

  auto hash = FileHash(path);
  auto start = DWORD(header);
  const auto ffs_dir = reinterpret_cast<FFS_Dir*>(header->dir_offset + start);
  auto head = reinterpret_cast<const DWORD*>(ffs_dir->nodes[hash % FFS_BucketCount] + start);

  while (*head) {
    auto curr_dir = reinterpret_cast<const WIN32_FIND_DATA*>(*head + start);
    if ((curr_dir->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
      __debugbreak();

    auto parent = reinterpret_cast<const WIN32_FIND_DATA*>(curr_dir->dwReserved0 + start);
    if (MatchesDirChain(start, parent, path))
      return curr_dir;
    // move to next node with the same hash.
    ++head;
  }
  // no more nodes with same hash.
  return nullptr;
}

const WIN32_FIND_DATA* GetLeaf(const WIN32_FIND_DATA* dot_node, const std::wstring& name) {
  DWORD group_id = dot_node->dwReserved0;
  auto curr  = dot_node + 1;
  while (curr->dwReserved0 == group_id) {
    if (name == curr->cFileName)
      return curr;
    ++curr;
  }
  return nullptr;
}

const WIN32_FIND_DATA* GetNode(const FFS_Header* header, const std::wstring& path) {
  if (path.size() < 3)
    return nullptr;
  if (path[1] != L':')
    return nullptr;
  if (path[path.size() - 1] == L'\\') {
    auto rez = path.substr(0, path.size() - 1);
    return GetDirectory(header, rez);
  }

  auto trail = path.rfind(L'\\');
  if (trail == std::wstring::npos)
    return nullptr;
  auto dir = path.substr(0, trail);
  auto leaf = path.substr(trail + 1);
  auto w32fd = GetDirectory(header, dir);
  if (!w32fd)
    return nullptr;
  if (!w32fd->dwReserved0)
    return nullptr;                      // $$$ fix this.
  return GetLeaf(w32fd, leaf);
}

int Testing(const FFS_Header* header) {
  auto fd1 = GetDirectory(header, L"f:\\src\\g0\\src\\athena");
  auto fd2 = GetNode(header, L"f:\\src\\g0\\src\\cc\\layers\\image_layer.h");
  auto fd3 = GetNode(header, L"f:\\src\\g0\\src\\chrome\\app\\resources\\terms\\");
  return 0;
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

    Testing(reinterpret_cast<FFS_Header*>(start));

  } __except (ExceptionFilter(GetExceptionInformation(), start, kMaxSize)) {
    // Probably ran out of memory.
    return 3;
  }

  return RunMainUILoop();
}
