@echo off
REM DDCBrightness 安装脚本
REM 需要以管理员身份运行

echo ========================================
echo   DDCBrightness 安装程序
echo ========================================
echo.

REM 检查管理员权限
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo [错误] 请以管理员身份运行此脚本!
    echo 右键点击此文件 -^> 以管理员身份运行
    pause
    exit /b 1
)

set DRIVER_DIR=%~dp0build\Release\x64
set DRIVER_SYS=%DRIVER_DIR%\DDCBrightness.sys
set DRIVER_INF=%DRIVER_DIR%\DDCBrightness.inf
set SERVICE_EXE=%DRIVER_DIR%\DDCBrightnessService.exe

REM 检查文件是否存在
if not exist "%DRIVER_SYS%" (
    echo [错误] 未找到驱动文件: %DRIVER_SYS%
    echo 请先编译项目 (Release x64 配置)
    pause
    exit /b 1
)

if not exist "%SERVICE_EXE%" (
    echo [错误] 未找到服务文件: %SERVICE_EXE%
    echo 请先编译项目 (Release x64 配置)
    pause
    exit /b 1
)

echo.
echo 步骤 1: 检查测试签名模式...
bcdedit /enum {current} | findstr /i "testsigning.*Yes" >nul 2>&1
if %errorLevel% neq 0 (
    echo [警告] 测试签名模式未启用!
    echo.
    echo 驱动需要签名才能加载。开发阶段需要启用测试签名:
    echo   bcdedit /set testsigning on
    echo   然后重启计算机
    echo.
    set /p CONTINUE="是否继续安装? (y/n): "
    if /i not "%CONTINUE%"=="y" (
        exit /b 1
    )
)

echo.
echo 步骤 2: 安装驱动...
REM 使用 pnputil 安装驱动
pnputil /add-driver "%DRIVER_INF%" /install
if %errorLevel% neq 0 (
    echo [警告] pnputil 安装可能需要重启或手动操作
)

echo.
echo 步骤 3: 添加上层过滤驱动注册表项...
REM 将 DDCBrightness 添加为 Monitor 类的上层过滤驱动
reg query "HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e96e-e325-11ce-bfc1-08002be10318}" /v UpperFilters >nul 2>&1
if %errorLevel% equ 0 (
    echo 已存在 UpperFilters, 追加 DDCBrightness...
    REM 注意: REG_MULTI_SZ 追加值较复杂, 使用 PowerShell
    powershell -Command "$key = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e96e-e325-11ce-bfc1-08002be10318}'; $val = (Get-ItemProperty -Path $key -Name UpperFilters).UpperFilters; if ($val -notcontains 'DDCBrightness') { $val += 'DDCBrightness'; Set-ItemProperty -Path $key -Name UpperFilters -Value $val -Type MultiString; Write-Host 'DDCBrightness 已添加到 UpperFilters' } else { Write-Host 'DDCBrightness 已在 UpperFilters 中' }"
) else (
    echo 创建 UpperFilters...
    reg add "HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e96e-e325-11ce-bfc1-08002be10318}" /v UpperFilters /t REG_MULTI_SZ /d DDCBrightness /f
)

echo.
echo 步骤 4: 安装用户态服务...
"%SERVICE_EXE%" install
if %errorLevel% neq 0 (
    echo [警告] 服务安装可能失败
)

echo.
echo 步骤 5: 启动服务...
net start DDCBrightnessService 2>nul
if %errorLevel% neq 0 (
    echo [信息] 服务可能需要重启后才能启动
)

echo.
echo ========================================
echo   安装完成!
echo ========================================
echo.
echo 如果 Windows 快捷设置中未出现亮度滑块,
echo 请尝试以下操作:
echo   1. 重启计算机
echo   2. 检查设备管理器中的显示器设备
echo   3. 运行 ddc_test.exe 验证 DDC/CI 可用性
echo.
echo 如需卸载, 请运行 uninstall.cmd
echo.
pause
