@echo off
rem bbport-kbm installer: run it from the Bloodborne PC folder (where BloodborneLauncher.exe is).
chcp 65001 >nul
setlocal
cd /d "%~dp0"
if not exist "out\bb-probe.exe" (
    echo This is not the Bloodborne PC folder: out\bb-probe.exe was not found.
    echo Это не папка Bloodborne PC: не найден out\bb-probe.exe.
    goto :fail
)
if not exist "kbm\SDL3.dll" (
    echo kbm\SDL3.dll is missing: extract the whole archive here.
    echo Нет kbm\SDL3.dll: распакуйте архив целиком.
    goto :fail
)
findstr /m /c:"bbport kbm" "out\SDL3.dll" >nul 2>&1
if errorlevel 1 (
    if not exist "out\SDL3_real.dll" copy /y "out\SDL3.dll" "out\SDL3_real.dll" >nul || goto :busy
) else (
    if not exist "out\SDL3_real.dll" (
        echo out\SDL3.dll is already the proxy but out\SDL3_real.dll is missing.
        echo Restore the original SDL3.dll from the Bloodborne PC archive and run this again.
        goto :fail
    )
)
copy /y "kbm\SDL3.dll" "out\SDL3.dll" >nul || goto :busy
echo.
echo Installed. Run BloodborneControls.exe to set up the keys, then start the game as usual.
echo Установлено. Настройка клавиш: BloodborneControls.exe, игра запускается как обычно.
echo In game: F7 = mouse capture on/off, F8 = reload settings.
pause
exit /b 0
:busy
echo Could not write to out\ - close the game and try again.
echo Не удалось записать в out\ - закройте игру и повторите.
:fail
pause
exit /b 1
