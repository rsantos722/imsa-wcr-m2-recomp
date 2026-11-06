#!/usr/bin/env python3
"""Check the built launcher's actual Windows application manifest resources."""
import ctypes
from ctypes import wintypes
from pathlib import Path
import sys
import xml.etree.ElementTree as ET


def main():
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.LoadLibraryExW.argtypes = [wintypes.LPCWSTR, wintypes.HANDLE, wintypes.DWORD]
    kernel.LoadLibraryExW.restype = wintypes.HMODULE
    kernel.FindResourceW.argtypes = [wintypes.HMODULE, ctypes.c_void_p, ctypes.c_void_p]
    kernel.FindResourceW.restype = wintypes.HRSRC
    kernel.SizeofResource.argtypes = [wintypes.HMODULE, wintypes.HRSRC]
    kernel.SizeofResource.restype = wintypes.DWORD
    kernel.LoadResource.argtypes = [wintypes.HMODULE, wintypes.HRSRC]
    kernel.LoadResource.restype = wintypes.HGLOBAL
    kernel.LockResource.argtypes = [wintypes.HGLOBAL]
    kernel.LockResource.restype = ctypes.c_void_p
    kernel.FreeLibrary.argtypes = [wintypes.HMODULE]
    kernel.FreeLibrary.restype = wintypes.BOOL

    # LOAD_LIBRARY_AS_DATAFILE: inspect resources without running launcher code
    # or loading its dependent DLLs. Windows reads manifest resource ID 1.
    module = kernel.LoadLibraryExW(str(Path(sys.argv[1]).resolve()), None, 2)
    if not module:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        resource = kernel.FindResourceW(module, 1, 24)  # RT_MANIFEST
        if not resource:
            raise AssertionError("Launcher has no embedded application manifest")
        size = kernel.SizeofResource(module, resource)
        data = kernel.LockResource(kernel.LoadResource(module, resource))
        if not size or not data:
            raise ctypes.WinError(ctypes.get_last_error())
        root = ET.fromstring(ctypes.string_at(data, size))
        ns = {"asm": "urn:schemas-microsoft-com:asm.v1"}
        identities = root.findall("asm:dependency/asm:dependentAssembly/asm:assemblyIdentity", ns)
        assert any(i.get("name") == "Microsoft.Windows.Common-Controls"
                   and i.get("version") == "6.0.0.0" for i in identities), (
            "Launcher must request themed Common Controls v6"
        )
        awareness = root.find(".//{http://schemas.microsoft.com/SMI/2016/WindowsSettings}dpiAwareness")
        assert awareness is not None and "PerMonitorV2" in awareness.text, (
            "Launcher must enable per-monitor v2 DPI scaling"
        )
    finally:
        kernel.FreeLibrary(module)
    print("Launcher embeds themed controls and per-monitor v2 DPI manifest")


if __name__ == "__main__":
    main()
