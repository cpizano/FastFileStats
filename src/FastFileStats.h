#pragma once

enum FFS_Consts {
  FFS_kVersion = 1,
  FFS_BucketCount = 1543,
  FFS_kMagic = 0x8855bed,
};

struct FFS_Header {
  DWORD magic;
  DWORD version;
  DWORD status;
  DWORD num_nodes;
  DWORD num_dirs;
  DWORD bytes;
  DWORD root_offset;
  DWORD pad0;
  DWORD hash_tbl[FFS_BucketCount];
};

enum FFS_Status {
  FFS_kBooting        = 0,
  FFS_kInProgress     = 1,
  FFS_kError          = 2,
  FFS_kUpdating       = 3,
  FFS_kFinished       = 4,
  FFS_kFrozen         = 5,
};
