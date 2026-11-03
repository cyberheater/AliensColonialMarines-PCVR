ACM-VR launcher
===============

The launcher starts the normal, unmodified ACM.exe and loads the VR mod into it.
Nothing in the game's own folders needs to change.

Setting it up in Steam
----------------------
1. In Steam, right-click Aliens: Colonial Marines > Properties > General > Launch Options.
2. Replace what's there with:

   "C:\Program Files (x86)\Steam\steamapps\common\Aliens Colonial Marines\ACMVR\acmvr_launcher.exe" %command%

   (Use the real path if this folder is somewhere else. Keep the quotes.)
3. Press Play as usual.

You don't need -ResX / -ResY any more. The launcher asks SteamVR for its per-eye resolution
and sets the game window to two eyes side by side. Start SteamVR before the game. If SteamVR
isn't running, the launcher uses the last size that worked. To set the size yourself, add
-ResX=... -ResY=... after %command%, and the launcher will leave them alone.
-windowed and -nostartupmovies are added automatically.

Playing without VR: clear the launch options again.

Important: turn OFF the Steam overlay for this game
---------------------------------------------------
Steam > right-click Aliens: Colonial Marines > Properties > General >
"Enable the Steam Overlay while in-game" = off.
In VR the Steam overlay puts its own panel in the headset and makes the game pause
(a "Resume" menu that only goes away when you click the game window). The launcher
window shows a warning if the mod saw the overlay in the last session.

The old install in Binaries\Win32
---------------------------------
While the old mod file d3d9.dll is still in ...\Binaries\Win32, the game keeps loading that
copy. The launcher sees this and stays out of the way. To switch fully to the launcher, delete
these from ...\Aliens Colonial Marines\Binaries\Win32:

   d3d9.dll   dxvk_d3d9.dll   dxvk.conf   openvr_api.dll   ACM_d3d9.log
   acmvr.ini  acmvr.log   acmvr_*.txt   acmvr_crash.dmp   acmvr_capture\ (folder)

Your settings (including the gun calibration) have already been copied into this folder.

Files in this folder
--------------------
acmvr_launcher.exe  starts the game and loads the mod
acmvr.dll           the VR mod
dxvk_d3d9.dll       DXVK (Direct3D 9 on Vulkan), dxvk.conf = its settings
openvr_api.dll      SteamVR interface
acmvr.ini           mod + launcher settings ([Launcher] section: GamePath, AutoResolution, ExtraArgs)
acmvr.log, acmvr_launcher.log, ACM_d3d9.log, acmvr_capture\   logs and captures (written here now)

If the launcher can't start the game, acmvr_launcher.log says why.
