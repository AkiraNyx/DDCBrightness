@echo off
REM DDCBrightness 本地一键构建脚本
REM 前置要求: Visual Studio 2022 + WDK + WiX v5

echo ========================================
echo   DDCBrightness 构建脚本
echo ========================================
echo.

set CONFIG=Release
set PLATFORM=x64
set BUILD_DIR=%~dp0build\%CONFIG%\%PLATFORM%

REM 查找 MSBuild
set MSBUILD=
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe 2^>nul`) do set MSBUILD=%%i

if "%MSBUILD%"=="" (
    echo [错误] 未找到 MSBuild。请安装 Visual Studio 2022。
    exit /b 1
)

echo 使用 MSBuild: %MSBUILD%
echo.

REM 构建驱动
echo [1/5] 构建驱动...
"%MSBUILD%" driver\DDCBrightness.vcxproj /p:Configuration=%CONFIG% /p:Platform=%PLATFORM% /v:minimal
if %errorLevel% neq 0 (
    echo [错误] 驱动构建失败
    exit /b 1
)

REM 构建服务
echo [2/5] 构建服务...
"%MSBUILD%" service\DDCBrightnessService.vcxproj /p:Configuration=%CONFIG% /p:Platform=%PLATFORM% /v:minimal
if %errorLevel% neq 0 (
    echo [错误] 服务构建失败
    exit /b 1
)

REM 构建测试工具
echo [3/5] 构建测试工具...
"%MSBUILD%" tools\DDCTest.vcxproj /p:Configuration=%CONFIG% /p:Platform=%PLATFORM% /v:minimal
if %errorLevel% neq 0 (
    echo [错误] 测试工具构建失败
    exit /b 1
)

REM 创建测试证书并签名
echo [4/5] 创建测试证书并签名驱动...
call scripts\sign-driver.cmd "%BUILD_DIR%\DDCBrightness.sys"
if %errorLevel% neq 0 (
    echo [警告] 驱动签名失败 (可在安装前手动签名)
)

REM 构建 MSI
echo [5/5] 构建 MSI 安装包...
where wix >nul 2>&1
if %errorLevel% neq 0 (
    echo [警告] WiX 未安装, 跳过 MSI 构建
    echo   安装: dotnet tool install --global wix
    goto :done
)

wix build installer\Package.wxs ^
    -d BuildDir="%BUILD_DIR%" ^
    -d CertFile="%BUILD_DIR%\DDCBrightness-test.cer" ^
    -d InfFile="driver\DDCBrightness.inf" ^
    -d Version="1.0.0.0" ^
    -arch x64 ^
    -o "%BUILD_DIR%\DDCBrightness-Setup.msi"

if %errorLevel% neq 0 (
    echo [警告] MSI 构建失败
) else (
    echo MSI 已生成: %BUILD_DIR%\DDCBrightness-Setup.msi
)

:done
echo.
echo ========================================
echo   构建完成!
echo ========================================
echo.
echo 输出目录: %BUILD_DIR%
echo.
dir /b "%BUILD_DIR%\*.sys" "%BUILD_DIR%\*.exe" "%BUILD_DIR%\*.msi" 2>nul
echo.
