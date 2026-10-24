# 文件取件（FilePickup）

> 把任意文件藏进一张图片，换一个地方用 5 位口令码取回。

<!-- 徽章占位：发布后替换 123qewafdfg / file-pickup -->
![版本](https://img.shields.io/badge/version-0.2.0-blue)
![平台](https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-lightgrey)
![许可证](https://img.shields.io/badge/license-MIT-green)

> **声明**：本项目仅供技术交流与个人在合法合规范围内传输自有数据使用。
> 与所接入的平台（网盘、频道、动态）无隶属关系，接口随时可能变更或失效，不另行通知。

> **官方网站：预留位（待补充）**
> 后续上线后在此填入：官网地址 / 文档站 / 下载页 / 联系方式。
> 占位格式：`https://example.com`（发布时替换本行）

---

## 这是什么

**文件取件**是一个 Windows 桌面小工具：它把你要传的内容加密后**藏进一张普通图片**，再把这张图片上传到网盘或内容平台，最后由后端登记一个 **5 位口令码**。对方只要拿到口令码，就能在另一台机器上把原文件取回来。

适合的场景：跨设备、跨人传一些不方便直接发的东西——文件、压缩包、图片、一段文本。

- 全程本地加密，密钥不上传
- 载体就是一张能正常打开的图片，平台上看起来只是一张图
- 口令码只有 5 位，方便口述或手抄

## 功能

| 模块 | 说明 |
| --- | --- |
| **制作** | 选择文件/图片/文本 → 本地加密 → 生成载体图片 → 上传平台 → 得到 5 位口令码 |
| **提取** | 输入口令码 → 拉回载体图片 → 本地解密还原，自动校验完整性 |
| **历史记录** | 本机保留每次制作/提取的条目，可回查口令码与文件 |
| **账号管理** | 管理各平台的登录态（小飞机网盘、QQ 频道、哔哩哔哩） |
| **检查更新** | 从后端读取新版信息，跳转下载（不静默安装、不自我替换） |

图片处理细节：

- 支持图片压缩（静图转 WebP，质量 75，**只有更小才采用**）
- 生成后**回读校验**，确认载体可恢复
- 大文件自动分片，按分片上传，提取时按序重组

## 工作原理

```text
本地文件
   │  ① 压缩（可选，图片转 WebP）
   │  ② 加密（XChaCha20-Poly1305 流式加密，密钥本地生成）
   │  ③ 装箱（TMSHARE1 容器，分片 + 头部元数据）
   ▼
载体图片（把装箱数据附加到图片尾部，图片本身仍然可正常显示）
   │  ④ 上传到平台（网盘直链 / 频道附件 / 动态图片）
   ▼
平台链接  ──⑤ 后端登记──▶  5 位口令码
                                  │
     对方输入口令码 ──⑥ 查询──▶ 拿到链接
                                  │
   下载载体图片 ──⑦ 解析尾部 ──▶ 解密 ──▶ 原文件（并校验完整性）
```

安全要点：

- 加密在本地完成，服务端只保存**链接与元数据**，不保存文件内容，也拿不到密钥
- 口令码是可枚举的短码，因此**载荷本身是加密的**——拿到口令码才能下载到密文，没有密钥仍然打不开
- 载体被平台重新编码（压缩/裁剪）时会破坏尾部数据，所以对会重编码的平台使用固定规格承载图

## 下载

发布产物在 [Releases](https://github.com/123qewafdfg/file-pickup/releases) 页面：

| 产物 | 适用 | 大小 |
| --- | --- | --- |
| `FilePickup-v0.2.0-Setup.exe` | **推荐**，双击安装，自动建快捷方式，可在设置里卸载 | 34.82 MiB |
| `FilePickup-v0.2.0-Portable.zip` | 免安装，解压即用（便携版） | 64.18 MiB |

所有产物的校验值见 [release/SHA256SUMS.txt](release/SHA256SUMS.txt)。

> **单文件版不随 Releases 发布**：它把 Qt 库封进 exe 内部、运行时才解包，无法满足 LGPLv3 的
> "允许用户替换 Qt 库"要求，因此仅作者自用，不对外分发。

> 系统要求：Windows 10 1809+ / Windows 11，x64。安装版为当前用户安装，不写注册表以外的系统位置，无需管理员权限。

## 快速上手

1. 安装并打开程序，在左侧「账号管理」里选一个平台并登录
2. 进入「制作」，把文件拖进窗口（或点「图片 / 视频 / 文件 / 文字」）
3. 点「选择发放平台」→ 确认上传
4. 上传完成后会得到一个 **5 位口令码**，把它发给对方
5. 对方打开同一个程序 → 「提取」→ 输入口令码 → 自动下载并还原

## 构建

### 环境要求

| 组件 | 版本 | 说明 |
| --- | --- | --- |
| Windows | 10 / 11 x64 | 仅支持 Windows |
| Visual Studio | 2022 Build Tools（MSVC v143） | 需要 C++ 桌面开发工具集 |
| Qt | 6.5 及以上（验证于 6.8.3 msvc2022_64） | 需要 Quick / QuickControls2 / Network / Svg / Concurrent |
| CMake | 3.21+ | 建议使用 Qt 自带或独立安装的 CMake |
| Ninja | — | 构建后端 |
| libsodium | 1.0.18+ | 需要**已编译**的 `libsodium.lib` + `libsodium.dll`，以及源码头文件 |
| FluentUI | 1.7.7 | QML 组件库（MIT），以子目录方式静态链接 |
| Python | 3.11（仅打包用） | 发布包内携带 embeddable 版，用于运行网络脚本 |

### 依赖路径

`CMakeLists.txt` 里的三个路径支持三级覆盖，**不用改源码**：

```text
1) 命令行：-DFLUENTUI_ROOT=... -DLIBSODIUM_ROOT=... -DLIBSODIUM_SOURCE=...
2) 环境变量：FLUENTUI_ROOT / LIBSODIUM_ROOT / LIBSODIUM_SOURCE
3) 默认值：作者本机路径（首次构建大概率需要覆盖）
```

`LIBSODIUM_ROOT` 目录里需要同时存在 `libsodium.lib` 与 `libsodium.dll`；
`LIBSODIUM_SOURCE` 指向 libsodium 源码根（取其 `src/libsodium/include`）。

### 构建命令

```powershell
# 生成 + 构建（MSVC 环境由脚本自动载入）
.\build.ps1                 # Debug
.\build.ps1 -Config Release # Release

# 自测
.\build\Release\cangjian_tests.exe
```

首次配置也可以手工执行：

```powershell
cmake -S . -B build\Release -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DFLUENTUI_ROOT=<FluentUI 源码目录> `
  -DLIBSODIUM_ROOT=<已编译 libsodium 目录> `
  -DLIBSODIUM_SOURCE=<libsodium 源码目录>
cmake --build build\Release
```

### 打包（可选）

`package-release.ps1` 会重建 Release、部署 Qt 运行库与 VC143 CRT、携带便携 Python，并产出：

- `portable/`：免安装目录
- `藏笺-PC-免安装.zip`：便携压缩包
- `single/藏笺.exe`：单文件版

安装包使用 Inno Setup 7 编译，脚本见 `installer/FilePickup.iss`。

> 注意：打包脚本里的工具路径（Qt、Inno Setup、VS Build Tools）按本机位置写死，属于作者机器的发布流程，其他环境请自行调整。

## 目录结构

```text
├─ src/
│  ├─ core/         加解密、TMSHARE1 载体、分片、压缩、后端协议、日志、安全存储
│  ├─ app/          应用控制器（QML 与核心库之间的胶水层）
│  ├─ qq/           QQ 频道扫码登录与频道 API
│  ├─ ui/           圆形揭示动画（主题切换）
│  └─ main.cpp      入口与无界面自检钩子
├─ qml/             QML 界面（制作 / 提取 / 历史 / 账号 / 设置）
├─ scripts/         运行时 Python 网络脚本（编入 Qt Resource，运行时释放）
├─ assets/          图标与承载图
├─ resources/       应用图标等 Windows 资源
├─ tests/           核心自测（加密、容器、分片、压缩、后端协议）
├─ third_party/     miniz（zip 解压）
├─ tools/launcher/  单文件启动器源码
├─ installer/       Inno Setup 安装脚本
├─ licenses/        LGPLv3 / GPLv3 许可证全文
├─ THIRD_PARTY_NOTICES.txt  第三方组件与许可证声明
└─ release/         发布产物与校验值（不参与版本库）
```

## 支持平台

| 平台 | 用途 | 说明 |
| --- | --- | --- |
| 小飞机网盘 | 文件载体直链 | 上传后返回可下载直链 |
| QQ 频道 | 附件/图文 | 需要扫码登录 |
| 哔哩哔哩 | 动态图片承载 | 使用固定规格承载图，避免 CDN 重编码 |

平台接口可能随时变化，脚本层（`scripts/`）做了隔离，便于单独修补。

## 隐私与安全

- 加密在本地完成，密钥随载体元数据一起封装（由口令派生之外的本地随机密钥），**不会上传**
- 服务端只保存「口令码 → 链接 + 元数据」，不保存文件内容
- 平台登录态保存在本机用户目录，使用 Windows DPAPI 加密
- 程序不含遥测；「检查更新」只在点击时发起一次请求

发现安全问题请见 [SECURITY.md](SECURITY.md)（或直接通过仓库 Issues 私密反馈渠道联系作者）。

## 常见问题

见 [docs/常见问题.md](docs/常见问题.md)。

## 路线图

- [x] Windows 客户端（制作 / 提取 / 历史 / 账号）
- [x] 多平台载体（网盘直链 / 频道 / 动态图片）
- [x] 应用内检查更新
- [x] 安装包体积优化（LZMA2 solid，34.8 MiB）
- [ ] **官方网站上线**（预留位，见本文顶部）
- [ ] 更多平台接入
- [ ] 命令行版本

## 许可证与第三方组件

本项目代码以 [MIT](LICENSE) 协议开源，Copyright (c) 2026 123qewafdfg。

随程序分发的第三方组件及其许可证见 **[THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt)**：

| 组件 | 许可证 | 用途 | 分发形式 |
| --- | --- | --- | --- |
| [Qt 6.8.3](https://www.qt.io/) | **LGPLv3** | 界面与网络 | 动态链接（独立 DLL） |
| [libsodium](https://libsodium.org/) | ISC | 流式加密 | 动态链接（DLL） |
| [FluentUI 1.7.7](https://github.com/zhuzichu520/FluentUI) | MIT | QML 组件库 | 静态链接 |
| [aria2](https://aria2.github.io/) | **GPLv2** | 下载加速 | 独立可执行文件（子进程） |
| [miniz](https://github.com/richgel999/miniz) | MIT | zip 解压 | 源码编译 |
| [Python 3.11](https://www.python.org/) | PSF | 运行时脚本 | 嵌入式运行时目录 |

### Qt 合规（LGPLv3）

为满足 LGPLv3 的要求，本项目做到：

1. **动态链接**：Qt 以独立 `Qt6*.dll` 随程序安装，不静态链接进主程序
2. **可替换**：安装目录（默认 `%LOCALAPPDATA%\Programs\FilePickup`）里的 Qt DLL 可直接替换，程序下次启动即生效
3. **许可证文本**：安装目录附带 `THIRD_PARTY_NOTICES.txt` 与 `licenses\LGPL-3.0.txt`、`licenses\GPL-3.0.txt`
4. **源码获取方式**：见 THIRD_PARTY_NOTICES.txt 第 1 节（官方下载地址）
5. **未修改 Qt 源码**

> 因此本项目**不依赖开源 Qt**，也可选择闭源；如需闭源分发，请继续遵守上述 5 条，或购买 Qt 商业许可。

### 关于 aria2（GPLv2）

aria2 以**独立可执行文件**（`aria2c.exe`）随包提供，由主程序以子进程调用，属于 GPL 所称的
"单纯聚合"，不会使本项目整体受 GPLv2 约束。其许可证全文与源码地址见 THIRD_PARTY_NOTICES.txt 第 7 节。

## 免责声明

本工具仅供个人在合法合规范围内传输自有数据使用。请勿用于侵犯他人隐私、传播违法内容或规避平台规则。使用本工具产生的一切后果由使用者自行承担。
