@echo off
REM 创建测试证书并签名驱动
REM 用法: sign-driver.cmd <驱动文件路径>

set DRIVER_FILE=%~1
if "%DRIVER_FILE%"=="" (
    echo 用法: sign-driver.cmd ^<驱动文件路径^>
    exit /b 1
)

set CERT_NAME=DDCBrightness Test
set CERT_STORE=PrivateCertStore
set BUILD_DIR=%~dp1

REM 查找 signtool
set SIGNTOOL=
for /f "usebackq delims=" %%i in (`dir /b /s "C:\Program Files (x86)\Windows Kits\10\bin\*\x64\signtool.exe" 2^>nul ^| sort /r`) do (
    if not defined SIGNTOOL set SIGNTOOL=%%i
)

if "%SIGNTOOL%"=="" (
    echo [错误] 未找到 signtool.exe。请安装 Windows SDK 或 WDK。
    exit /b 1
)

REM 查找 makecert 或使用 PowerShell
echo 创建测试证书...

REM 使用 PowerShell 创建自签名证书
powershell -Command ^
    "$existing = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq 'CN=%CERT_NAME%' };" ^
    "if ($existing) { Write-Host '测试证书已存在'; exit 0 };" ^
    "$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=%CERT_NAME%' -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(3);" ^
    "Write-Host \"证书已创建, 指纹: $($cert.Thumbprint)\";" ^
    "Export-Certificate -Cert $cert -FilePath '%BUILD_DIR%DDCBrightness-test.cer' | Out-Null;" ^
    "Write-Host '证书已导出到 %BUILD_DIR%DDCBrightness-test.cer'"

if %errorLevel% neq 0 (
    echo [错误] 证书创建失败
    exit /b 1
)

REM 签名驱动
echo 签名驱动: %DRIVER_FILE%
"%SIGNTOOL%" sign /s My /n "%CERT_NAME%" /fd SHA256 /td SHA256 "%DRIVER_FILE%"

if %errorLevel% neq 0 (
    echo [错误] 驱动签名失败
    exit /b 1
)

echo 驱动签名成功!
echo.
echo 注意: 安装此驱动需要先启用测试签名模式:
echo   bcdedit /set testsigning on
echo   然后重启计算机
