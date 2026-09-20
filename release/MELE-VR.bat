@echo off
setlocal enableextensions enabledelayedexpansion
title MELE VR
color 0B

echo(
echo  --------------------------------------------------
echo    MELE VR - Mass Effect Legendary Edition ^(ME1^)
echo  --------------------------------------------------
echo(

set "TARGET=%~dp0"
if "!TARGET:~-1!"=="\" set "TARGET=!TARGET:~0,-1!"

if not exist "!TARGET!\MassEffect1.exe" (
  echo   ERROR: MassEffect1.exe is not next to this installer.
  echo(
  echo   This bat has to run FROM your game folder, not from the zip
  echo   or your Downloads folder. Fix:
  echo(
  echo     1. Extract the whole MELE-VR.zip ^(MELE-VR.bat, dxgi.dll,
  echo        openxr_loader.dll, MELEVR.ini - all four files^).
  echo     2. Copy or move ALL FOUR into your game's Win64 folder:
  echo          ...\Mass Effect Legendary Edition\Game\ME1\Binaries\Win64
  echo     3. Run MELE-VR.bat from THAT folder.
  echo(
  echo   ^(Not sure where that is: right-click Mass Effect 1 in Steam -^>
  echo   Manage -^> Browse Local Files, then open Game\ME1\Binaries\Win64.^)
  echo(
  pause
  exit /b 1
)

set "MISSING="
if not exist "!TARGET!\dxgi.dll" set "MISSING=1"
if not exist "!TARGET!\openxr_loader.dll" set "MISSING=1"
if defined MISSING (
  echo   ERROR: dxgi.dll and/or openxr_loader.dll aren't in this folder.
  echo(
  echo   You only copied PART of the mod here. Go back to the extracted
  echo   MELE-VR folder and copy dxgi.dll and openxr_loader.dll in too -
  echo   all four files ^(this .bat, both .dll files, MELEVR.ini^) need to
  echo   sit together in your Win64 folder.
  echo(
  pause
  exit /b 1
)

REM --- game must be closed before touching its files ---
tasklist /fi "imagename eq MassEffect1.exe" 2>nul | find /i "MassEffect1.exe" >nul
if not errorlevel 1 (
  echo   Please CLOSE Mass Effect first, then run this again.
  echo(
  pause
  exit /b 1
)

REM  WRITE-ACCESS CHECK + SELF-ELEVATE
REM
REM  If this folder needs Administrator rights to write to (common when Steam
REM  is under C:\Program Files (x86)), don't fail with a cryptic copy error -
REM  test for it up front and relaunch elevated so the user only
REM  sees ONE Windows permission popup instead of a confusing error message.
set "WTEST=!TARGET!\.melevr_writetest"
(echo test) > "!WTEST!" 2>nul
if not exist "!WTEST!" (
  net session >nul 2>&1
  if errorlevel 1 (
    echo   This folder needs Administrator rights to install into
    echo   ^(this happens when Steam is under Program Files^).
    echo   Requesting permission - click YES on the popup...
    echo(
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -WorkingDirectory '%~dp0' -Verb RunAs" >nul 2>&1
    exit /b 0
  ) else (
    echo   ERROR: cannot write to this folder even as Administrator.
    echo   Check it isn't read-only, or that antivirus isn't blocking it.
    echo(
    pause
    exit /b 1
  )
)
del /f /q "!WTEST!" >nul 2>&1

echo   Found ME1 at:
echo     !TARGET!
echo(

REM --- explicit menu (no blind toggle). MELEVR.ini itself is NOT a good "already configured"
REM     signal any more - it ships INSIDE the zip now (drop-in-place model), so it exists the
REM     instant the user extracts, before the wizard has ever run. Use expectedResX instead: it
REM     ships as 0 and is only ever set by the wizard below, so nonzero = configured before. ---
set "INSTALLED=0"
set "CURVRMODE="
if exist "!TARGET!\MELEVR.ini" (
  for /f "tokens=3" %%V in ('findstr /b /r "expectedResX" "!TARGET!\MELEVR.ini" 2^>nul') do if not "%%V"=="0" set "INSTALLED=1"
  REM Current vrMode, so a re-run can default the mode picker to what's already configured
  REM instead of silently resetting to Stereo (this is also how the bat pre-selects the [x]
  REM default below, so hitting Enter on a reconfigure KEEPS your mode unless you change it).
  for /f "tokens=3" %%V in ('findstr /b /r "vrMode" "!TARGET!\MELEVR.ini" 2^>nul') do set "CURVRMODE=%%V"
)
if "!INSTALLED!"=="1" ( echo   MELE VR is currently INSTALLED here. ) else ( echo   MELE VR is NOT installed here yet. )
echo(
echo   What do you want to do?
echo     [1] Install / Reconfigure MELE VR
echo     [2] Uninstall MELE VR
echo     [3] Cancel  ^(do nothing^)
echo(
set /p "ACTION=Enter 1-3 [1]: "
if not defined ACTION set "ACTION=1"
if "!ACTION!"=="3" ( echo   Cancelled. Nothing was changed. & pause & exit /b 0 )
if "!ACTION!"=="2" (
  if "!INSTALLED!"=="0" ( echo   Nothing to uninstall - MELE VR isn't configured here. & pause & exit /b 0 )
  goto UNINSTALL
)
goto INSTALL


REM  INSTALL
:INSTALL
echo  --------------------------------------------------
echo    INSTALL
echo  --------------------------------------------------
echo(
echo   This installer turns HDR off for you - it's the #1 thing that
echo   makes the headset image blue or doubled.
echo(

set "FRESH=1"
if "!INSTALLED!"=="1" set "FRESH=0"

set "VRMODE=2"
set "MNAME=Stereo"
set "QNAME=Sharp"
set "RX=5120"
set "RY=5120"

REM Default the mode picker to whatever's already configured (if anything), so hitting
REM Enter on a reconfigure KEEPS your mode - you only change it if you type a new number.
set "DEFPICK=1"
if "!CURVRMODE!"=="2" set "DEFPICK=1"
if "!CURVRMODE!"=="0" set "DEFPICK=2"
if "!CURVRMODE!"=="1" set "DEFPICK=3"
if "!CURVRMODE!"=="3" set "DEFPICK=4"

echo   Choose your VR mode ^(mode + resolution are picked together, every run^):
echo(
echo     [1] Stereo  - true 3D, full render per eye   ^(recommended^)
echo     [2] Mono    - flat image + head tracking ^(lightest^)
echo     [3] AER     - alternate-eye, lighter, can flicker
echo     [4] DIBR    - depth reprojection
echo(
set /p "PICK=Enter 1-4 [!DEFPICK!]: "
if not defined PICK set "PICK=!DEFPICK!"
if "!PICK!"=="1" ( set "VRMODE=2" & set "MNAME=Stereo" )
if "!PICK!"=="2" ( set "VRMODE=0" & set "MNAME=Mono" )
if "!PICK!"=="3" ( set "VRMODE=1" & set "MNAME=AER" )
if "!PICK!"=="4" ( set "VRMODE=3" & set "MNAME=DIBR" )
echo(
echo   Selected: !MNAME!.
echo(

if "!VRMODE!"=="2" ( set "RX=4096" & set "RY=4250" & set "QNAME=Balanced" )
if "!VRMODE!"=="0" ( set "RX=5120" & set "RY=2880" & set "QNAME=Sharp" )
if "!VRMODE!"=="1" ( set "RX=3328" & set "RY=3536" & set "QNAME=Sharp" )
if "!VRMODE!"=="3" ( set "RX=3328" & set "RY=3536" & set "QNAME=Sharp" )

echo   Choose your image quality ^(picks the sharpness your GPU can handle^):
echo(
if "!VRMODE!"=="2" (
  echo     [1] Low         - 2k.
  echo     [2] Performance - 3k.
  echo     [3] Balanced    - 4k.  ^(recommended^)
  echo     [4] Sharp       - 5k.
  echo     [5] Max         - 6k.
  echo(
  set "QDEF=3"
  set /p "QPICK=Enter 1-5 [3]: "
) else (
  echo     [1] Low         - below native, softer, big fps gain.
  echo     [2] Performance
  echo     [3] Balanced
  echo     [4] Sharp       - ^(recommended^)
  echo     [5] Max         - 6k resolution.
  echo(
  set "QDEF=4"
  set /p "QPICK=Enter 1-5 [4]: "
)
if not defined QPICK set "QPICK=!QDEF!"

if "!VRMODE!"=="2" (
  if "!QPICK!"=="1" ( set "RX=2048"  & set "RY=2124"  & set "QNAME=Low" )
  if "!QPICK!"=="2" ( set "RX=3072"  & set "RY=3188"  & set "QNAME=Performance (native)" )
  if "!QPICK!"=="3" ( set "RX=4096"  & set "RY=4250"  & set "QNAME=Balanced" )
  if "!QPICK!"=="4" ( set "RX=5120"  & set "RY=5312"  & set "QNAME=Sharp" )
  if "!QPICK!"=="5" ( set "RX=6144"  & set "RY=6374"  & set "QNAME=Max" )
)
REM Mono (=0): still one wide 16:9 frame shown to both eyes (no per-eye render), so it stays wide.
if "!VRMODE!"=="0" (
  if "!QPICK!"=="1" ( set "RX=2560" & set "RY=1440" & set "QNAME=Low" )
  if "!QPICK!"=="2" ( set "RX=3840" & set "RY=2160" & set "QNAME=Performance" )
  if "!QPICK!"=="3" ( set "RX=4608" & set "RY=2592" & set "QNAME=Balanced" )
  if "!QPICK!"=="4" ( set "RX=5120" & set "RY=2880" & set "QNAME=Sharp" )
  if "!QPICK!"=="5" ( set "RX=6144" & set "RY=3456" & set "QNAME=Max" )
)
if "!VRMODE!"=="1" (
  if "!QPICK!"=="1" ( set "RX=1280" & set "RY=1360" & set "QNAME=Low" )
  if "!QPICK!"=="2" ( set "RX=1920" & set "RY=2040" & set "QNAME=Performance" )
  if "!QPICK!"=="3" ( set "RX=2560" & set "RY=2720" & set "QNAME=Balanced" )
  if "!QPICK!"=="4" ( set "RX=3328" & set "RY=3536" & set "QNAME=Sharp" )
  if "!QPICK!"=="5" ( set "RX=4096" & set "RY=4352" & set "QNAME=Max" )
)
if "!VRMODE!"=="3" (
  if "!QPICK!"=="1" ( set "RX=1280" & set "RY=1360" & set "QNAME=Low" )
  if "!QPICK!"=="2" ( set "RX=1920" & set "RY=2040" & set "QNAME=Performance" )
  if "!QPICK!"=="3" ( set "RX=2560" & set "RY=2720" & set "QNAME=Balanced" )
  if "!QPICK!"=="4" ( set "RX=3328" & set "RY=3536" & set "QNAME=Sharp" )
  if "!QPICK!"=="5" ( set "RX=4096" & set "RY=4352" & set "QNAME=Max" )
)
echo(
echo   Selected: !QNAME! ^(!RX!x!RY!^).
echo(

REM --- back up a non-mod dxgi.dll if one is somehow already here (rare in the drop-in-place
REM     model - the mod's own dxgi.dll usually arrived via the same extraction) ---
if exist "!TARGET!\dxgi.dll.premelevr.bak" goto SKIPBAK
findstr /m "MELEVR" "!TARGET!\dxgi.dll" >nul 2>&1
if errorlevel 1 copy /y "!TARGET!\dxgi.dll" "!TARGET!\dxgi.dll.premelevr.bak" >nul 2>&1
:SKIPBAK

echo   Mod files present.

powershell -NoProfile -Command "$p='!TARGET!\MELEVR.ini'; (Get-Content -LiteralPath $p) -replace '^^vrMode = .*','vrMode = !VRMODE!' | Set-Content -LiteralPath $p" >nul 2>&1
echo   Wrote to MELEVR.ini


REM --- the resolution watchdog: tell the mod what got set, so it can warn in the in-game
REM     menu if the game ever resets it (opening the in-game video options does). ---
powershell -NoProfile -Command "$p='!TARGET!\MELEVR.ini'; $c=Get-Content -LiteralPath $p; if($c -match '^^expectedResX'){$c=$c -replace '^^expectedResX = .*','expectedResX = !RX!' -replace '^^expectedResY = .*','expectedResY = !RY!'}else{$c+=@('expectedResX = !RX!','expectedResY = !RY!')}; Set-Content -LiteralPath $p $c" >nul 2>&1

set "RESKEYX=sfr2ResX"
set "RESKEYY=sfr2ResY"
if "!VRMODE!"=="0" ( set "RESKEYX=monoResX" & set "RESKEYY=monoResY" )
if "!VRMODE!"=="1" ( set "RESKEYX=aerResX" & set "RESKEYY=aerResY" )
if "!VRMODE!"=="3" ( set "RESKEYX=dibrResX" & set "RESKEYY=dibrResY" )
powershell -NoProfile -Command "$p='!TARGET!\MELEVR.ini'; $c=Get-Content -LiteralPath $p; if($c -match '^^!RESKEYX! '){$c=$c -replace '^^!RESKEYX! = .*','!RESKEYX! = !RX!' -replace '^^!RESKEYY! = .*','!RESKEYY! = !RY!'}else{$c+=@('!RESKEYX! = !RX!','!RESKEYY! = !RY!')}; Set-Content -LiteralPath $p $c" >nul 2>&1
echo   Synced !MNAME! resolution into MELEVR.ini (!RESKEYX!/!RESKEYY!)

set "GS=!TARGET!\..\..\BioGame\Config\GamerSettings.ini"
powershell -NoProfile -Command "$p='!GS!'; $nl=[char]13+[char]10; $d=Split-Path -Parent $p; if(-not(Test-Path -LiteralPath $d)){New-Item -ItemType Directory -Force -Path $d | Out-Null}; if(-not(Test-Path -LiteralPath $p)){Set-Content -LiteralPath $p ('[SystemSettings]'+$nl+$nl+'[HDR]'+$nl)}; $c=Get-Content -LiteralPath $p -Raw; if($c -notmatch '(?m)^^\[SystemSettings\]'){$c=$c.TrimEnd()+$nl+$nl+'[SystemSettings]'+$nl}; if($c -notmatch '(?m)^^\[HDR\]'){$c=$c.TrimEnd()+$nl+$nl+'[HDR]'+$nl}; $sys=@('ResX=!RX!','ResY=!RY!','BorderlessWindow=True','Fullscreen=False','DynamicShadows=False','FilmGrain=False','MotionBlur=False'); foreach($kv in $sys){$n=$kv.Split('=')[0]; if($c -match ('(?m)^^'+$n+'=')){$c=[regex]::Replace($c,'(?m)^^'+$n+'=[^^\r\n]*',$kv)} else {$c=([regex]'(?m)^^\[SystemSettings\]').Replace($c,'[SystemSettings]'+$nl+$kv,1)}}; if($c -match '(?m)^^DesiredRange='){$c=[regex]::Replace($c,'(?m)^^DesiredRange=[^^\r\n]*','DesiredRange=DynamicRange_SDR')} else {$c=([regex]'(?m)^^\[HDR\]').Replace($c,'[HDR]'+$nl+'DesiredRange=DynamicRange_SDR',1)}; Set-Content -LiteralPath $p $c; $v=Get-Content -LiteralPath $p -Raw; $need=$sys+@('DesiredRange=DynamicRange_SDR'); $bad=@($need | Where-Object { $v -notmatch ('(?m)^^'+[regex]::Escape($_)+'\s*$') }); if($bad.Count -eq 0){'ok'}else{'FAIL '+($bad -join ' ')}" >"%TEMP%\melevr_gs.txt" 2>nul
set /p "GSOK="<"%TEMP%\melevr_gs.txt"
del "%TEMP%\melevr_gs.txt" >nul 2>&1
if /i "!GSOK!"=="ok" (
  echo   Resolution set to !RX!x!RY!. Shadows/film grain/motion blur/HDR off.
) else (
  echo   WARNING: could not write/verify GamerSettings.ini ^(!GSOK!^).
  echo(
  echo   The file is created automatically now, so this usually means a permissions
  echo   problem: run this installer as administrator and try again.
  echo(
  echo   Until then, set the resolution + turn HDR AND MOTION BLUR off yourself in
  echo   the in-game video menu - motion blur ON makes reflections/flares shake in VR.
)

REM  Frame-rate smoothing OFF. This lives in BIOEngine.ini, a DIFFERENT file than the
REM  GamerSettings.ini block above, and is NOT exposed in the in-game video menu at all.
REM  SmoothFrameRate=TRUE caps the game at MaxSmoothedFrameRate (60), whatever the
REM  resolution or VR mode - forced off every run.
REM  Depth of field is deliberately NOT forced here any more: turning it off makes the
REM  game lose colour and look washed out (tested in ME1 and ME2). It stays the player's
REM  choice in the mod menu, off by default.
set "BE=!TARGET!\..\..\BioGame\Config\BIOEngine.ini"
powershell -NoProfile -Command "$p='!BE!'; if(Test-Path -LiteralPath $p){$c=Get-Content -LiteralPath $p -Raw; $c=[regex]::Replace($c,'(?m)^SmoothFrameRate=\w+','SmoothFrameRate=FALSE'); Set-Content -LiteralPath $p $c; if((Get-Content -LiteralPath $p -Raw) -match 'SmoothFrameRate=FALSE'){'ok'}else{'FAIL'}}else{'MISSING'}" >"%TEMP%\melevr_be.txt" 2>nul
set /p "BEOK="<"%TEMP%\melevr_be.txt"
del "%TEMP%\melevr_be.txt" >nul 2>&1
if /i not "!BEOK!"=="ok" echo   NOTE: could not turn frame-rate smoothing off ^(BIOEngine.ini !BEOK!^) - not critical.

echo(
echo  --------------------------------------------------
echo    DONE. MELE VR is installed.
echo  --------------------------------------------------
echo(
echo    1. Put your headset on and launch Mass Effect ^(ME1^).
echo(
echo    2. INSERT = mod menu,   R = recenter,   K = first person.
echo(
echo    Run this installer again any time to change settings or uninstall.
echo(
pause
exit /b 0


REM  UNINSTALL
:UNINSTALL
echo  --------------------------------------------------
echo    MELE VR is already installed here.
echo  --------------------------------------------------
echo(
set /p "GO=Uninstall MELE VR? [Y/N] "
if /i not "!GO!"=="Y" ( echo   Cancelled. Nothing was changed. & pause & exit /b 0 )

del /f /q "!TARGET!\dxgi.dll" >nul 2>&1
del /f /q "!TARGET!\openxr_loader.dll" >nul 2>&1
if exist "!TARGET!\dxgi.dll.premelevr.bak" move /y "!TARGET!\dxgi.dll.premelevr.bak" "!TARGET!\dxgi.dll" >nul
echo   Removed the mod ^(dxgi.dll + openxr_loader.dll^). Mass Effect will
echo   run completely vanilla now.

echo(
set /p "DELS=Also delete your settings and logs? [Y/N] "
if /i "!DELS!"=="Y" (
  if exist "!TARGET!\MELEVR.ini" del /f /q "!TARGET!\MELEVR.ini" >nul 2>&1
  if exist "!TARGET!\MELEVR_Log.txt" del /f /q "!TARGET!\MELEVR_Log.txt" >nul 2>&1
  if exist "%LOCALAPPDATA%\MELEVR" rmdir /s /q "%LOCALAPPDATA%\MELEVR" >nul 2>&1
  if exist "%USERPROFILE%\Documents\MELEVR" rmdir /s /q "%USERPROFILE%\Documents\MELEVR" >nul 2>&1
  echo   Deleted MELEVR.ini and logs.
) else (
  echo   Kept your MELEVR.ini so your settings survive a reinstall.
)

echo(
echo   MELE VR uninstalled.
echo(
pause
exit /b 0
