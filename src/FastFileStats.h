#pragma once

struct FFS_Header {
  DWORD magic;
  DWORD version;
  DWORD status;
  DWORD num_nodes;
  DWORD num_dirs;
  DWORD bytes;
  DWORD pad1;
  DWORD pad2;
};

enum FFS_Magic {
  FFS_kMagic = 0x8855bed
};

enum FFS_Version {
  FFS_kVersion = 1
};

enum FFS_Status {
  FFS_kBooting        = 0,
  FFS_kInProgress     = 1,
  FFS_kError          = 2,
  FFS_kUpdating       = 3,
  FFS_kFinished       = 4,
  FFS_kFrozen         = 5,
};

