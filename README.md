# DDCBrightness - Windows 外置显示器亮度驱动

通过 DDC/CI 协议使 Windows 内置亮度滑块能够控制外置显示器亮度。

## 快速安装 (用户)

从 [Releases](../../releases) 页面下载最新的 `DDCBrightness-Setup.msi`，以管理员身份运行即可。

### 安装步骤

1. 下载 `DDCBrightness-Setup.msi`
2. 右键 → 以管理员身份运行
3. 安装程序会自动完成以下操作:
   - 安装驱动和服务文件
   - 安装测试证书
   - 注册上层过滤驱动
   - 安装并启动亮度控制服务
4. **首次安装需启用测试签名模式**:
   ```cmd
   bcdedit /set testsigning on
   ```
   然后重启计算机
5. 重启后 Windows 快捷设置中出现亮度滑块

> **注意**: 当前版本使用测试签名，桌面右下角会显示「测试模式」水印。正式签名版本将在后续发布。

### 前置条件

- Windows 10/11 x64
- 外置显示器支持 DDC/CI 协议 (在显示器 OSD 菜单中确认已开启)

### 卸载

控制面板 → 程序和功能 → DDCBrightness → 卸载

---

## 项目概述

Windows 台式机连接外置显示器时，系统快捷设置不会显示亮度滑块。本项目通过 KMDF 过滤驱动注册亮度设备接口，配合用户态服务通过 DDC/CI 协议实际控制显示器，使 Windows 原生亮度滑块可用。

### 架构

```
Windows 亮度滑块 (快捷设置)
        │
   IOCTL_VIDEO_SET_DISPLAY_BRIGHTNESS
        │
        ▼
  DDCBrightness.sys          (KMDF 上层过滤驱动 - 内核态)
  ├─ 注册 GUID_DEVINTERFACE_BRIGHTNESS
  ├─ 处理亮度 IOCTL
  └─ 反向调用通知服务
        │
        ▼
  DDCBrightnessService.exe   (Windows 服务 - 用户态)
  ├─ 自动枚举 DDC/CI 显示器
  ├─ 亮度范围映射 (DDC/CI <-> 百分比)
  └─ SetMonitorBrightness (DDC/CI)
        │
        ▼
  外置显示器 (通过 DDC/CI 接收亮度命令)
```

### 亮度映射

Windows 亮度滑块使用 0-100 百分比值。DDC/CI 亮度 (VCP code 0x10) 的范围因显示器而异:
- 大多数显示器: 0-100 (1:1 直接映射)
- 部分显示器: 0-255 或其他范围 (自动转换)

映射公式:
- Windows → DDC: `ddc_value = min + (percent * (max - min)) / 100`
- DDC → Windows: `percent = ((ddc_value - min) * 100) / (max - min)`

---

## 开发者指南

### 开发环境

1. **Visual Studio 2022 Community** (免费)
   - 安装时勾选「使用 C++ 的桌面开发」工作负载

2. **Windows Driver Kit (WDK)**
   - 下载: https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk
   - 选择与 VS 版本匹配的 WDK
   - 安装后需安装 WDK VS 扩展

3. **WiX v5** (构建 MSI 安装包)
   ```cmd
   dotnet tool install --global wix
   ```

### 构建

#### 方式一: 一键构建脚本

```cmd
build.cmd
```

自动完成编译、签名、打包 MSI。

#### 方式二: Visual Studio

1. 打开 `DDCBrightness.sln`
2. 选择 `Release | x64`
3. 生成解决方案 (Ctrl+Shift+B)

#### 方式三: GitHub Actions

推送到 `main` 分支或创建 `v*` 标签自动触发构建，产物可在 Actions 页面下载。创建标签时自动生成 GitHub Release 草稿。

### 构建产物

位于 `build/Release/x64/`:
| 文件 | 说明 |
|------|------|
| `DDCBrightness.sys` | 内核过滤驱动 |
| `DDCBrightnessService.exe` | 用户态服务 |
| `ddc_test.exe` | DDC/CI 测试工具 |
| `DDCBrightness-Setup.msi` | MSI 安装包 |
| `DDCBrightness-test.cer` | 测试证书 |

### 手动安装 (开发调试)

```cmd
REM 启用测试签名
bcdedit /set testsigning on
REM 重启

REM 签名驱动
scripts\sign-driver.cmd build\Release\x64\DDCBrightness.sys

REM 安装
install.cmd
```

### 调试

**服务调试模式** — 在控制台直接查看日志:
```cmd
DDCBrightnessService.exe console
```

**驱动调试** — 使用 [DebugView](https://learn.microsoft.com/en-us/sysinternals/downloads/debugview) 查看内核调试输出:
1. 以管理员身份运行 DebugView
2. 启用「Capture Kernel」
3. 查看 `DDCBrightness:` 前缀的输出

**DDC/CI 测试** — 验证显示器连接:
```cmd
ddc_test.exe              # 枚举所有显示器
ddc_test.exe --set 50     # 测试设置亮度到 50%
```

### 发布流程

1. 更新版本号 (INF、WiX)
2. 创建 Git 标签: `git tag v1.0.0`
3. 推送: `git push origin v1.0.0`
4. GitHub Actions 自动构建并创建 Release 草稿
5. 在 GitHub Release 页面审核并发布

### 关于驱动签名

| 阶段 | 签名方式 | 用户体验 |
|------|----------|----------|
| 开发/预览版 | 测试签名 | 需启用测试模式，桌面有水印 |
| 正式发行版 | EV 证书 + WHQL | 无感安装，无水印 |

正式发行需要:
1. 购买 EV 代码签名证书
2. 注册 Microsoft 硬件开发者中心
3. 提交驱动进行 Attestation Signing 或 WHQL 认证

## 项目结构

```
DDCBrightness/
├── .github/workflows/
│   └── build.yml                # GitHub Actions CI/CD
├── common/
│   └── public.h                 # 驱动和服务共享的定义
├── driver/
│   ├── driver.h                 # 驱动内部定义
│   ├── driver.c                 # 驱动入口和设备管理
│   ├── brightness.c             # 亮度 IOCTL 处理
│   ├── DDCBrightness.inf        # 驱动安装配置
│   └── DDCBrightness.vcxproj    # WDK 项目文件
├── service/
│   ├── service.h                # 服务头文件
│   ├── service.cpp              # 服务实现
│   └── DDCBrightnessService.vcxproj
├── tools/
│   ├── ddc_test.cpp             # DDC/CI 测试工具
│   └── DDCTest.vcxproj
├── installer/
│   ├── Package.wxs              # WiX MSI 安装包定义
│   └── License.rtf              # 许可证 (RTF 格式)
├── scripts/
│   └── sign-driver.cmd          # 测试签名脚本
├── build.cmd                    # 本地一键构建脚本
├── install.cmd                  # 手动安装脚本
├── uninstall.cmd                # 手动卸载脚本
├── DDCBrightness.sln            # VS 解决方案
└── README.md
```

## 常见问题

**Q: 安装后亮度滑块没出现**
- 确认已重启计算机
- 检查设备管理器 → 显示器 → 属性 → 驱动程序，确认过滤驱动已加载
- 以管理员打开 PowerShell，检查 UpperFilters:
  ```powershell
  Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e96e-e325-11ce-bfc1-08002be10318}" -Name UpperFilters
  ```

**Q: 亮度滑块出现但拖动无效果**
- 运行 `DDCBrightnessService.exe console` 查看日志
- 运行 `ddc_test.exe` 确认 DDC/CI 正常
- 确认服务正在运行: `sc query DDCBrightnessService`

**Q: 显示器不支持 DDC/CI**
- 在显示器 OSD 菜单中查找 DDC/CI 选项并开启
- 尝试使用 HDMI 或 DisplayPort 连接 (VGA 可能不支持)
- 部分低端显示器确实不支持 DDC/CI

## 许可证

MIT License
