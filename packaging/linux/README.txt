IMSA World Championship Racing (M2 Recomp)

To play, run ./imsa-wcr-m2-recomp from a terminal in this folder. If your file manager
opens it as text, choose "Run" or enable running scripts in its settings. On first
launch, select your IMSA Racing ISO image.

This package uses your system's GTK 3 libraries. Use the AppImage release if your
system libraries are incompatible.

Keep this whole folder together. Everything the game stores stays in this folder:

  userdata  your saves, screenshots, settings and the game files extracted from
            your disc (created on first run)
  bin, lib  the game and the files it needs to run
  share     icon and desktop entry
  licenses  license information

If you cannot write to this folder (in /opt, for example), the game stores everything
in ~/.local/share/imsa-wcr-m2-recomp instead. The AppImage always stores it there, so
copy your saves across if you switch between the two.

To update, extract the new version and copy your old userdata folder into it.
Your saves, settings, and installed game files will carry over.
