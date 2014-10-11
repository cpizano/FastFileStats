///////////////////////////////////////////////////////////////////////////////////////////////////
// Server main.
//
// The FFS server job is to keep an up-to date Shared section with information about a directory
// tree. The format of the shared section is mostly compatible with what FindFirstFile and
// FindNextFile return with some important caveats.
// 
// Clients are expected to map this shared section and use it to speed up directory enumeration
// and file stat'ing.  The shared section can be big, in the order of 30 MB for the chromium
// source tree, at its initial state. As mutations happen to the tree, it can grow all the way to
// the value of kMaxSharedSize.
//
// The basic block of the shared section is a lite version of WIN32_FIND_DATA. The SDK version of
// this structure is over 512 bytes (!) so the lite version only extends to the string size of the 
// cFileName member. Which in average gives us 9x smaller footprint for large trees.
//
// When the server starts it does an initial pass enumerating every file in the tree given as input
// and then enters in monitor mode using ReadDirectoryChangesW.
//
// Along with the basic blocks, there are 3 main navigational structures in the shared section:
// 1- Directory hashtable : given a directory name hash, it will point to the set of directories
//                          basic blocks that have the same hash.
// 2- Parent linked list  : given a directory basic block, it points to the parent directory.
// 3- Sibling linked list : given a basic block it will give you the next basic block of the same
//                          directory.
//
// #2 is constructed using dwReserved0
// #3 is constructed using dwReserved1 and dwReserved0
// #1 is stand-alone and lives at the end of the initial pass.
//
// With these 3 primitives we expect to be able to do all querys and updates needed.
//
// Here is an ilustrated example: for c:\\DirA\DirB\fileX and c:\\DirA\FileY
//
//   hash-table
//      +---+                                            hash-row
//     0|   |                                 +--+--+------+--+-----------------+--+
//      +---+                                 |  |  |      |  |                 |0 |
//     1|   |-------------------------------->+--+--+------+--+-----------------+--+
//      +---+                                                |
//     2|   |                 hash-row                       |
//      +---+        +--+--+--+--+--------+--+----+--+       |
//     3|   |------->|  |  |  |  |        |  |    |0 |       |
//      +---+        +--+--+--+--+--------+--+----+--+       |
//     4|   |                  |                             |
//      +---+                  |                             |
//     5|   |              +---v-------+ dwReserved0   +-----v------+ dwReserved0  
//      +---+              | dot-dir   |--->---+       | dot-dir    |->-------+     
//      |   |              +---+-------+       |       +-----+------+         |    
//      |   |                  |               |             |                |       
//      |   |                  |dwReserved1    |             |dwReserved1     |      
//      |   |                  |               |             |                |      
//      |   |              +---v-------+       +----- >+-----v------+         |  
//      |   |              |           |       |       |            |         |  
// 1543 |   |              |  fileX    |--->---+       |  dirB      |->-------+  
//      +---+              +---+-------+       |       +-----+------+         | 
//                             |               |             |                |         
//                             |               |             |                |         
//                         +---v-------+       |       +-----v------+         |        root
//                         |           |       |       |  fileY     |->-------+--->+------------+
//                         |           +--->---+       +------------+              | c:\\dirA   |
//                         +-----------+                                           +------------+
//
//  The hash table points to the start of each hash-row vector (which is the set of all entities
//  with the same hash, and it is terminated by a 0. Each hash-row vecrtor entry points to the
//  first entry (dot file) of each directory. Each entry (WIN32_FIND_DATA) has two pointers: one
//  to the next entry on the same directory and one to the entry that represents the parent.
//
//  Each entry cFileName member only contains the path component. Only the root entry (which bwt
//  is fake) contains the full volume path.
//

#include "stdafx.h"

#include <string>
#include <tuple>
#include <vector>

#include "resource.h"
#include "FastFileStats.h"

#define PARANOID 1

// All shared data must fit in 300MB.
const DWORD kMaxSharedSize = 1024 * 1024 * 300;

const auto kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                      FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION |
                      FILE_NOTIFY_CHANGE_SIZE;

template <typename T, typename U>
T VerifyNot(T actual, U error) {
  if (actual != error)
    return actual;

  volatile ULONG err = ::GetLastError();
  __debugbreak();
  __assume(0);
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

WIN32_FIND_DATA* AdvanceNext(WIN32_FIND_DATA* current) {
  DWORD len = (wcslen(current->cFileName) + 1) * sizeof(wchar_t);
  len = (len + 8 ) & ~7;
  current->dwReserved1 = len;
  return reinterpret_cast<WIN32_FIND_DATA*>(
      reinterpret_cast<BYTE*>(&current->cFileName[0]) + len);
}

const WIN32_FIND_DATA* AdvanceNext(const WIN32_FIND_DATA* current) {
  if (!current->dwReserved1)
    __debugbreak();
  return reinterpret_cast<const WIN32_FIND_DATA*>(
      reinterpret_cast<const BYTE*>(&current->cFileName[0]) + current->dwReserved1);
}

bool CreateFFS(BYTE* const start, DWORD size, const wchar_t* top_dir) {
  auto mem = start;
  auto header = reinterpret_cast<FFS_Header*>(mem);
  *header = FFS_Header{FFS_kMagic, FFS_kVersion, FFS_kBooting, 0, 0, 0};
  mem += sizeof(*header);

  typedef std::tuple<std::wstring, DWORD> Entry;

  std::vector<Entry> pending_dirs;
  std::vector<Entry> found_dirs;
  std::vector<DWORD> dir_offsets[FFS_BucketCount];

  DWORD all_count = 0;
  DWORD dir_count = 0;
  DWORD pending_fixes = 0;
  DWORD reparse_count = 0;

  // The first node is a fake node with the root so we don't have special cases.
  auto w32fd = reinterpret_cast<WIN32_FIND_DATA*>(mem);
  w32fd->dwFileAttributes = -1;
  w32fd->dwReserved0 = 0;
  w32fd->dwReserved1 = 0;
  wcscpy_s(w32fd->cFileName, top_dir);
  header->root_offset = DWORD(w32fd) - DWORD(start);

  pending_dirs.emplace_back(top_dir, header->root_offset);
  w32fd = AdvanceNext(w32fd);

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
      w32fd = AdvanceNext(w32fd);

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
        w32fd = AdvanceNext(w32fd);
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
  auto curr  = AdvanceNext(dot_node);
  while (curr->dwReserved0 == group_id) {
    if (name == curr->cFileName)
      return curr;
    curr = AdvanceNext(curr);
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

void UpdateModified(FFS_Header* header, WIN32_FIND_DATA* oldfd) {
  WIN32_FIND_DATA newfd;
  auto fff = ::FindFirstFileW(oldfd->cFileName, &newfd);
  ::FindClose(fff);
  int count = 0;

  if (oldfd->ftLastWriteTime.dwLowDateTime != newfd.ftLastWriteTime.dwLowDateTime)
    count += 1;
  if (oldfd->ftLastWriteTime.dwHighDateTime != newfd.ftLastWriteTime.dwHighDateTime)
    count += 2;
  if (oldfd->nFileSizeHigh != newfd.nFileSizeHigh)
    count += 4;
  if (oldfd->nFileSizeLow != newfd.nFileSizeLow)
    count += 8;
}

struct Context {
  FFS_Header* ffs_header;
  HANDLE top_dir;
  BYTE io_buff[1024 * 16];
};

void CALLBACK ChangesCompletionCB(DWORD error, DWORD bytes, OVERLAPPED* ov) {
  if (!bytes)
    return;
  auto ctx = reinterpret_cast<Context*>(ov->hEvent);
  static std::wstring root = std::wstring(reinterpret_cast<WIN32_FIND_DATA*>(
      ctx->ffs_header->root_offset + DWORD(ctx->ffs_header))->cFileName) + L"\\";

  auto fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(ctx->io_buff);
  if (!fni->FileNameLength)
    return;

  ctx->ffs_header->status = FFS_kUpdating;

  int count = 0;
  while (true) {
    ++count;
    // regarless of the notification, see if we have it.
    auto path = root + std::wstring(fni->FileName, fni->FileNameLength / sizeof(wchar_t));
    auto node = GetNode(ctx->ffs_header, path);

    switch (fni->Action) {
      case FILE_ACTION_ADDED:
        break;
      case FILE_ACTION_REMOVED:
        break;
      case FILE_ACTION_MODIFIED:
        UpdateModified(ctx->ffs_header, const_cast<WIN32_FIND_DATA*>(node));
        break;
      case FILE_ACTION_RENAMED_OLD_NAME:
        break;
      case FILE_ACTION_RENAMED_NEW_NAME:
        break;
    }

    if (!fni->NextEntryOffset)
      break;
    fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
        reinterpret_cast<BYTE*>(fni) + fni->NextEntryOffset);
  }

  // subscribe again.
  ::ReadDirectoryChangesW(ctx->top_dir, ctx->io_buff, sizeof(ctx->io_buff),
                          TRUE, kFilter,  NULL, ov, &ChangesCompletionCB);
}

bool StartWatchingTree(const wchar_t* dir, FFS_Header* ffs_header) {
  auto kShareAll = FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE;
  auto dir_handle = ::CreateFileW(dir, GENERIC_READ, kShareAll, 
      NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);

  if (dir_handle == INVALID_HANDLE_VALUE)
    return false;
  auto ctx = new Context {ffs_header, dir_handle};
  auto ov = new OVERLAPPED {0};
  ov->hEvent = HANDLE(ctx);
  if (!::ReadDirectoryChangesW(dir_handle, 
                               ctx->io_buff, sizeof(ctx->io_buff),
                               TRUE, kFilter,  NULL, ov, &ChangesCompletionCB))
    return false;
  return true;
}

int __stdcall wWinMain(HINSTANCE module, HINSTANCE, wchar_t* cc, int) {
  const wchar_t dir[] = L"f:\\src";

  auto mmap = ::CreateFileMappingW(
      INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_RESERVE, 0, kMaxSharedSize, L"ffs_(f)!src");
  auto start = reinterpret_cast<BYTE*>(
      ::MapViewOfFile(mmap, FILE_MAP_ALL_ACCESS, 0, 0, kMaxSharedSize));
  if (!start)
    return 1;

  __try {

    if (!StartWatchingTree(dir, reinterpret_cast<FFS_Header*>(start)))
      return 2;

    if (!CreateFFS(start, kMaxSharedSize, dir))
      return 3;

    Testing(reinterpret_cast<FFS_Header*>(start));

    while (true) {
      ::SleepEx(INFINITE, TRUE);
    }
    return 0;

  } __except (ExceptionFilter(GetExceptionInformation(), start, kMaxSharedSize)) {
    // Probably ran out of memory.
    return 6;
  }
}
