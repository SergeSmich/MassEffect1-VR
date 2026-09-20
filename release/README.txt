MELE VR  -  Mass Effect Legendary Edition (ME1) VR mod with 6DOF head tracking for Mass Effect
by Halcyon

Copyright (c) 2026 Halcyon.
Licensed under the GNU General Public License v3.0 - see LICENSE.txt. You are
free to use, modify, and redistribute this software under the terms of that
license.


INSTALL
1. Extract ALL FOUR files (MELE-VR.bat, dxgi.dll, openxr_loader.dll,
   MELEVR.ini) into the folder that has MassEffect1.exe - that's
   ...\Mass Effect Legendary Edition\Game\ME1\Binaries\Win64
2. Double-click  MELE-VR.bat  FROM THAT FOLDER.
3. It asks which VR mode you want (Stereo / Mono / AER / DIBR),
   then which image quality (Low / Performance / Balanced / Sharp /
   Max). Stereo + Balanced is the recommended
   default. You can change VR mode later in the in-game menu; a
   resolution/quality change needs re-running this installer.
4. Done.

If Windows needs Administrator rights to write into that folder
(common if Steam is under Program Files), the installer will ask
for permission automatically - just click Yes.

BEFORE YOU PLAY  -  IMPORTANT
Turn OFF HDR in Mass Effect's own video options. If you boot the game and it
gets stuck blue/doubled in the headset, that's HDR - go turn it off there.


CONTROLS
* INSERT    - open / close the mod menu
* R         - recenter your view
* K         - toggle first-person camera on/off
* P         - toggle depth pop on/off
* F1-F4     - instantly load profile 1 / 2 / 3 / 4 (rebindable in the menu,
              Profiles tab; press once to switch your whole settings preset)

You can switch VR mode (Mono / Stereo / AER / DIBR) any time
from the menu (a resolution change needs a game restart).

Depth of field is ON by default. You can turn it off in the menu under the
VR tab -> Graphics, but it can give the game a washed out look; a change
applies on the next launch.

UNINSTALL
Run  MELE-VR.bat AGAIN and follow instructions.

Or just delete dxgi.dll and openxr_loader.dll from the game's folder.
