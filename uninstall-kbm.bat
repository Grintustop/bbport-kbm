@echo off
rem bbport-kbm uninstaller: puts the original SDL3.dll back. input_config\ is kept.
chcp 65001 >nul
setlocal
cd /d "%~dp0"
if not exist "out\SDL3_real.dll" (
    echo Nothing to uninstall: out\SDL3_real.dll was not found.
    echo Нечего удалять: нет out\SDL3_real.dll.
    pause
    exit /b 0
)
move /y "out\SDL3_real.dll" "out\SDL3.dll" >nul || (
    echo Could not restore out\SDL3.dll - close the game and try again.
    echo Не удалось восстановить out\SDL3.dll - закройте игру и повторите.
    pause
    exit /b 1
)
echo Uninstalled: the original SDL3.dll is restored. Your bindings stay in input_config\.
echo Удалено: оригинальная SDL3.dll восстановлена. Настройки остались в input_config\.
pause
