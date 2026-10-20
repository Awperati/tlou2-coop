TLOU2 First Person, loaded without touching the game folder
==========================================================

This folder holds the first-person mod (TLOU2FirstPerson.asi) and a small launcher (fp_launcher.exe) that
loads it into the running game. Nothing is copied into the game folder: no winmm.dll, no ASI loader.

Use
  1. Start the game as usual.
  2. Double-click fp_launcher.exe (or start it before the game; it waits). About 8 seconds after the game
     window appears it loads the mod, then keeps watching and loads it again into a restarted game.
     Close the launcher window (or Ctrl+C) when you don't want it.
  One-shot: fp_launcher.exe --once   (load into the running game, then exit)
  Slower PC: fp_launcher.exe --delay 15

Turning it off
  The mod can't be unloaded from a running game. Quit the game; next time, don't run fp_launcher.exe.

Settings
  The mod's own settings and log (TLOU2FirstPerson.ini, TLOU2FirstPerson.log) live in this folder, next to the
  .asi. Its in-game menu saves there too.

With the co-op mod
  Both can run together: each launcher loads its own mod. They have not been tested together yet; both touch the
  camera, so try first person on its own first. If the game folder still has a winmm.dll ASI loader with this ASI,
  the launcher sees the mod already loaded and does nothing.
