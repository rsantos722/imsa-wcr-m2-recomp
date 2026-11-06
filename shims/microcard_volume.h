// microcard_volume.h — the guest-visible volume names of the storage
// boundary, in a header with no dependencies so both the host shim and tests
// can include it.
//
// WHAT "/" ACTUALLY LISTS ON PORTFOLIO. The root directory is not an on-media
// directory block. The file folio synthesises it on demand: `RootFileIO()`
// (Portfolio OS folios/file/driver.c)
// answers FILECMD_READDIR on a directory with no `fi_FileSystem` by walking
// `fileFolio.ff_Filesystems` in mount order and emitting ONE entry per MOUNTED
// FILESYSTEM:
//
//     de.de_FileName = theFS->fs.n_Name            // driver.c:251
//                                                  // fs.n_Name aliases
//                                                  // fs_FileSystemName
//                                                  // (folio.c:352) = mount name
//     de.de_Flags    = fs_RootDirectory->fi_Flags  // driver.c:259
//
// and a volume root's flags are set at mount time to
// FILE_IS_DIRECTORY | FILE_IS_FOR_FILESYSTEM (mount.c:514-517). The folio's own
// autodoc states the contract: "supplying a path of \"/\" to
// OpenDirectoryPath(). The entries that ReadDirectory() returns will correspond
// to all of the mounted file systems" (folios/file/autodocs.c:233-237).
//
// So a "/" listing holds mounted volume names and nothing else. The host
// therefore presents two volumes, in mount order:
//
//   "data"     — the read-only disc volume, whose root IS the asset tree.
//                Every guest asset path already uses this prefix
//                ("/data/tracks/...", "/data/ui/...").
//   M2_CARD_VOLUME — the writable MicroCard stand-in, inserted after the disc,
//                so it sorts last.
//
// WHY THE GUEST REJECTS EXACTLY ONE NAME. Portfolio mounts the developer
// host-link filesystem under the literal volume name "remote":
// `others/dipir/dev.hostfs.c:88`
//     err = MountHostFS(fd->fd_HWResource, "remote", &dirToken);
// with the failure string "Host: cannot mount /remote" at dev.hostfs.c:91.
// FindMicroCard@0x103ac (shooter2d) / @0x1faa8 (driving) enumerates "/", keeps
// every entry whose attr word has bits 0 and 22 set and whose name is NOT
// "remote", and — critically — KEEPS SCANNING after each acceptance
// (shooter2d `b 0x103ec` at 0x1044c; driving `bl 4:33` + `bge` at 0x1fb30), so
// the LAST such entry wins. The strcmp therefore excludes the developer's share
// and asks for real card-backed storage: the routine is deliberately
// volume-label-agnostic.
//
// WHY "card". The card's volume name on real hardware is whatever the card's
// own on-media volume identifier says — Portfolio copies `fs_FileSystemName`
// from the media (formatted via dl_VolumeIdentifier) — and the vendor tree
// supplies no default name for a MicroCard. So the name is HOST POLICY, and the
// single source-derived constraint is that it differ from "remote". Naming the
// host's simulated card "remote" makes the guest skip the only writable
// volume it was offered.
#pragma once

// The writable volume the host presents in "/", after the read-only disc.
#define M2_CARD_VOLUME "card"

// The one root-entry name the guest's strcmp rejects (see above). Kept as a
// literal so a future edit cannot silently make the card unreachable by naming
// it this.
#define M2_HOST_LINK_VOLUME "remote"

// The read-only disc volume's name in "/"; its root is the asset tree, so this
// is the prefix every guest asset path already carries.
#define M2_DISC_VOLUME "data"
