// filesystem_folio.h — filesystem folio (id 4) vectors used by `driving/game`.
//
// 12 distinct vectors, 144 calls. Ordinals 0/1 (OpenFile/CloseFile) are
// pinned. Ordinals 32..40 are now statically repinned from checked-in caller
// shapes rather than the shifted positional public vector list:
//
//   32 OpenDirectoryPath   33 ReadDirectory    34 CloseDirectory
//   35 OpenRawFile         36 CloseRawFile      37 ReadRawFile
//   38 WriteRawFile        39 SeekRawFile       40 GetRawFileInfo
//
// 32/33/34 are pinned by FN_FindMicroCard__1faa8: open path "/", enumerate
// (handle,&entry), close handle. 35/36/37/40 form a complete lifecycle in
// imsasound_loadfile@0x2c6cc; 38 is the write primitive used 20 times by
// writeRepNode@0x267a4; 39 is the freopen@0x74840 call with (handle,0,2).
#pragma once

#include "shim_abi.h"
#include "shim_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct m2_file m2_file_t;  // opaque host handle
typedef struct m2_dir m2_dir_t;    // opaque host directory-enumeration handle

enum {
    M2_FS_OPEN_DIRECTORY_PATH_ORDINAL = 32,
    M2_FS_READ_DIRECTORY_ORDINAL = 33,
    M2_FS_CLOSE_DIRECTORY_ORDINAL = 34,
    M2_FS_OPEN_RAW_FILE_ORDINAL = 35,
    M2_FS_CLOSE_RAW_FILE_ORDINAL = 36,
    M2_FS_READ_RAW_FILE_ORDINAL = 37,
    M2_FS_WRITE_RAW_FILE_ORDINAL = 38,
    M2_FS_SEEK_RAW_FILE_ORDINAL = 39,
    M2_FS_GET_RAW_FILE_INFO_ORDINAL = 40,
};

// Guest-visible directory-entry layout pinned by the FindMicroCard lift:
// attr word at +0 gates enumeration on TWO bits; NUL-terminated name at +36
// (compared via kernel:320 strcmp). Bytes +4..+35 are unobserved; the host
// zeroes them for determinism. Name capacity is bounded by the caller's
// stack window (entry buffer at r1+8, saved regs from r1+0x5c).
//
// The two gate bits, and why. `rlwinm`/`clrlwi` take MB/ME as MSB-FIRST bit
// numbers and PPC_MASK implements that (runtime/include/ppc_ctx.h:607-611:
// PPC_MASK(22,22) == 0x00000200, i.e. VALUE bit 9). So the shooter's gates
// decode as:
//   0x10404 `clrlwi. r12,r12,0x1f`        -> RLWINM(x,0,31,31)
//            -> PPC_MASK(31,31) == 0x1     -> value bit 0 = FILE_IS_DIRECTORY
//   0x10410 `rlwinm. r12,r12,0,0x16,0x16` -> PPC_MASK(0x16,0x16)
//            -> 0x200                      -> value bit 9 = FILE_USER_STORAGE_PLACE
// (driving's 0x1faec / 0x1faf4 are the same pair.) Both are named in the
// vendor header ws_root/src/includes/file/filesystem.h:174,181, and both are
// exactly what a volume root carries: mount.c:514 sets FILE_IS_DIRECTORY on
// every mounted volume root, and the Acrobat driver ORs
// FILE_USER_STORAGE_PLACE into it (drivers/file/acrobat/ops.c:598-601).
// The previous constant, 0x00400000, was NOT the bit the guest tests: it left
// the value-bit-9 gate clear on every entry, so FindMicroCard skipped the whole
// listing and returned 0 whatever the volume was called.
enum {
    M2_DIRENT_ATTR_OFF = 0,
    M2_DIRENT_NAME_OFF = 36,
    M2_DIRENT_NAME_CAP = 48,  // bytes incl. NUL terminator
};
#define M2_DIRENT_ATTR_IS_DIRECTORY (1u << 0)   // FILE_IS_DIRECTORY
#define M2_DIRENT_ATTR_USER_STORAGE (1u << 9)   // FILE_USER_STORAGE_PLACE

// --- volume model: what "/" actually lists ------------------------------------
// Full derivation and citations live in microcard_volume.h; the macros are
// shared with tests, which cannot include this header.
#include "microcard_volume.h"

// Portfolio FileOpenModes, the r5 argument of folio 4:35 OpenRawFile
// (Portfolio OS file/fileio.h).
// The shooter's own call sites are unambiguous: WriteMicroCardFile@0x10670 and
// writerawimage@0x102e4 pass FILEOPEN_WRITE_NEW, while ReadMicroCardFile@0x1074c
// and every read site pass FILEOPEN_READ.
enum {
    M2_FILEOPEN_READ = 0,
    M2_FILEOPEN_WRITE = 1,
    M2_FILEOPEN_WRITE_NEW = 2,
    M2_FILEOPEN_WRITE_EXISTING = 3,
    M2_FILEOPEN_READWRITE = 4,
    M2_FILEOPEN_READWRITE_NEW = 5,
    M2_FILEOPEN_READWRITE_EXISTING = 6,
};

// --- pinned ------------------------------------------------------------------
m2_file_t* m2_OpenFile(const char* name);                   // vector 0
m2_err_t m2_CloseFile(m2_file_t* f);                        // vector 1
m2_err_t m2_CreateAlias(const char* existing, const char* alias); // vector 10 (log-only)

// --- pinned by FindMicroCard call shape --------------------------------------
m2_dir_t* m2_OpenDirPath(const char* path);                 // selector 32
int32_t m2_ReadDir(m2_dir_t* d, void* entry_out);           // selector 33
m2_err_t m2_CloseDir(m2_dir_t* d);                          // selector 34

// --- direct host helper API ---------------------------------------------------
// These C helpers mirror host-side file operations and are retained for native
// callers. Their names are NOT the selector-35..40 table: guest selector
// marshalling lives in recomp_call_fs_dispatch, whose 35..40 identities are
// listed above. In particular, do not infer selector numbers for the setters
// below from declaration order or from the shifted public vector list.
int32_t m2_SeekRawFile(m2_file_t* f, int32_t offset, int32_t whence);
int32_t m2_GetRawFileInfo(m2_file_t* f, int32_t tag);
m2_err_t m2_SetRawFileAttrs(m2_file_t* f, uint32_t attrs);
m2_err_t m2_SetFileAttrs(const char* name, uint32_t attrs);
void m2_ClearRawFileError(m2_file_t* f);
m2_err_t m2_SetRawFileSize(m2_file_t* f, uint32_t size);

#ifdef __cplusplus
}  // extern "C"
#endif
