@echo off
REM DDCBrightness 卸载脚本
REM 需要以管理员身份运行

echo ========================================
echo   DDCBrightness 卸载程序
echo ========================================
echo.

REM 检查管理员权限
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo [错误] 请以管理员身份运行此脚本!
    pause
    exit /b 1
)

set SERVICE_EXE=%~dp0build\Release\x64\DDCBrightnessService.exe

echo 步骤 1: 停止并卸载服务...
net stop DDCBrightnessService 2>nul
if exist "%SERVICE_EXE%" (
    "%SERVICE_EXE%" uninstall
) else (
    sc delete DDCBrightnessService 2>nul
)

echo.
echo 步骤 2: 从 UpperFilters 中移除 DDCBrightness...
powershell -Command "$key = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e96e-e325-11ce-bfc1-08002be10318}'; try { $val = (Get-ItemProperty -Path $key -Name UpperFilters -ErrorAction Stop).UpperFilters; $val = $val | Where-Object { $_ -ne 'DDCBrightness' }; if ($val.Count -gt 0) { Set-ItemProperty -Path $key -Name UpperFilters -Value $val -Type MultiString } else { Remove-ItemProperty -Path $key -Name UpperFilters -ErrorAction SilentlyContinue }; Write-Host 'DDCBrightness 已从 UpperFilters 中移除' } catch { Write-Host 'UpperFilters 不存在或无需清理' }"

echo.
echo 步骤 3: 卸载驱动...
pnputil /delete-driver DDCBrightness.inf /uninstall 2>nul
if %errorLevel% neq 0 (
    echo [信息] 驱动可能已经被移除或需要重启
)

echo.
echo ========================================
echo   卸载完成!
echo ========================================
echo.
echo 请重启计算机以完成驱动卸载。
echo.
pause
