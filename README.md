# pbz-modkit

《影之刃零》（Phantom Blade Zero，PC / Steam 版）的非官方 Mod 平台：DLL 加载器、Mod管理器、开发 SDK，以及基于它做的几个 Mod（1v1 联机 PvP、CS 步枪）。

> 非官方项目，与游戏开发商无关。仓库不含任何游戏文件或资源，不解锁付费 DLC。
> 加载器每次启动自己找引擎地址，框架按名字读字段，普通的游戏更新不需要重新适配；只有 UE 引擎大版本升级才可能失效（当前是 UE 5.7）。使用 Mod 的风险自负，建议先备份存档。

▶ 联机 PvP 演示视频：[哔哩哔哩 BV1ERxQ6mEo1](https://www.bilibili.com/video/BV1ERxQ6mEo1/)

| 联机大厅 | 比赛中 |
|---|---|
| ![大厅](docs/images/pvp-lobby.jpg) | ![比赛](docs/images/pvp-fight.jpg) |
| **CS 步枪（AWP）** | **Mod管理器** |
| ![AWP](docs/images/sniper-awp.jpg) | ![Mod管理器](docs/images/modman.png) |

## 功能一览

**加载器（`version.dll`）**
- 作为 `version.dll` 代理放在 `PBZ.exe` 旁边，系统导出全部转发，不修改游戏文件。
- 在游戏主线程每帧回调 Mod，回调外面套了 SEH 保护：单个 Mod 出错只记日志，不会拖垮游戏。
- 按 `pbzmp\autorun.txt` 加载 Mod，也支持在运行中通过 `pbzmp\cmd.txt` 执行 `run <id>` / `stop <id>`，做热加载和热卸载（加载的是 DLL 副本，可以边改边编）。
- 统一接口：每帧 tick、下一帧执行（post）、日志、函数 Hook（MinHook，同一个目标只允许一个 Mod 挂，冲突时记日志）、全局热键（只在游戏窗口在前台时生效）。Mod 卸载时，它注册过的东西都会自动清理。
- 每次启动扫描 `PBZ.exe` 找到对象数组、名字池、ProcessEvent 和引擎 Tick（结果写进 `loader.log`），游戏更新后不用改地址。
- 能和别的 `version.dll` Mod 共存：把对方改名为 `version_chain.dll`，本加载器启动后会顺带加载它。

**Mod管理器（`Mod管理器.exe`）**
- 图形界面勾选启用的 Mod，也可以用命令行：`--list`、`--set a,b`、`--chain`。
- 保存前会检查前置 Mod 和最低版本、互斥的 Mod、Mod 需要的加载器接口版本，有问题时说明原因并拒绝保存。
- 发现外来的 `version.dll` 时，会提示一键串联。

**Mod**
- [`mods/pvp`](mods/pvp/README.md)：1v1 联机对战，支持 Steam P2P 或自建中转服务器。有房间大厅，可以选武器 / 杀器 / 外观，有 3 张擂台地图，比赛不读写存档，崩溃后可以重连。
- [`mods/sniper`](mods/sniper/README.md)：AWP / AK-47 / M4A4，CS2 手感，只能单机用。需要自备资源。
- [`sdk/hello`](sdk/README.md)：用框架写的示例，按名字读血量、架势、位置，没有一个内存地址。
- [`sdk/example`](sdk/README.md)：最小示例，演示 tick、hook 和热键。

## 目录结构

```
pbz-modkit/
├─ build.bat            一键构建，产物都放在 out\
├─ package.py           打玩家包 dist\pbz_pvp-v<版本>.zip
├─ loader/              加载器：main.cpp、host.h（Mod 接口）、proxy_exports.h
├─ modman/              Mod管理器
├─ framework/           UE 5.7 运行时访问，给 Mod 静态链接
│   ├─ ue.cpp/.h          对象表、FName、反射（类 / 属性 / 函数），引擎地址来自加载器
│   ├─ ue_call.h          ProcessEvent 的参数封装
│   └─ uex.cpp/.h         按名字找字段（Prop / Field）、属性、调用函数、界面、日志、存档保护开关
├─ sdk/                 Mod 开发文档和示例（hello：框架示例，example：最小示例）
├─ mods/
│   ├─ pvp/               联机 PvP：src/ server/ docs/ maps/
│   └─ sniper/            CS 步枪
├─ tools/               开发用：多开测试实例的启动 / 停止 / 列表脚本
└─ third_party/minhook/
```

依赖关系：Mod 只依赖 `loader/host.h`；要访问 UE 对象时，再链接 `framework/`。加载器和管理器不依赖任何 Mod。

## 安装（玩家）

| 版本 | 适配游戏版本（PBZ.exe 文件版本） |
|---|---|
| v1.4.1 | 同 v1.4。修复联机：加入方打房主没伤害、双方看不到对方攻击动作（双方都需更新）；同步杀器蓝光预警特效 |
| v1.4 | 不再绑定具体版本：加载器启动时自己找地址，字段按名字查（测试于 `CL-302256_JKS-17280`，2026-11-04） |
| v1.3 | `CL-302256_JKS-17280`，2026-11-04 更新后的版本（文件版本号没变，exe 修改日期为 11-04） |
| v1.2 | `CL-302256_JKS-17280`，11-04 更新之前 |

从 v1.4 起，游戏更新后一般不需要新版本。如果更新后 Mod 不工作，打开 `pbzmp\loader.log`：`engine:` 那行写着加载器找到的地址，`field ... not found` 表示游戏删掉或改名了某个字段，把日志发给作者即可。PBZ.exe 的文件版本可以在右键 → 属性 → 详细信息里查看。

1. 在 Steam 里右键游戏，选「管理 → 浏览本地文件」，进入 `platform\steam\`（`PBZ.exe` 所在的文件夹）。
2. 把发布包解压到这里，得到 `version.dll`、`Mod管理器.exe`、`pbzmp\`（每个 Mod 在里面有自己的文件夹，例如 `pbzmp\mp\`）。从旧版本升级时直接覆盖解压即可，原来的设置会被自动沿用。
3. 用 Mod管理器勾选要启用的 Mod 并保存，然后正常启动游戏。

卸载：删掉 `version.dll`、`Mod管理器.exe` 和 `pbzmp\` 即可。如果之前串联过别的 Mod，把 `version_chain.dll` 改回 `version.dll`。

PvP 的详细玩法和开服说明见 [mods/pvp/README.md](mods/pvp/README.md)。

## 从源码构建

需要：
- Visual Studio 2022（Build Tools 就够），带 x64 的 C++ 工具链
- Python 3（打包脚本、PvP 服务器和测试要用）
- 可选：Node.js，用来转换 sniper 的模型

```bat
build.bat            :: 编译全部：version.dll modman.exe mp.dll sniper.dll example.dll nettest.exe rsrv.exe
python package.py    :: 生成 dist\pbz_pvp-v<版本>.zip（版本取 mods/pvp/mp.txt 的 version=）
python package.py --full   :: 本地自测包，额外带 CS 步枪和本地资源，不要发布
```

`build.bat` 会自动调用 VS 2022 Build Tools 的 `vcvars64.bat`。如果你装的是其他版本，先在「x64 Native Tools Command Prompt」里运行它，或者修改脚本开头的 `VCVARS`。

开发时把 `out\` 里的 DLL 复制到游戏的 `platform\steam\pbzmp\<id>\`（例如 `pbzmp\mp\mp.dll`），再往 `pbzmp\cmd.txt` 写一行 `stop mp` / `run mp`，就能热重载，不用重启游戏。

## 测试

```bat
out\nettest.exe                                   :: 网络层：本机回环、模拟丢包抖动、双进程大厅测试
python mods\pvp\server\test_server.py             :: 中转服务器的协议、阶段、超时、限流测试
set PBZMP_CPP_SERVER=%CD%\out\rsrv.exe
python mods\pvp\server\test_server.py             :: 同一套测试，跑 C++ 版服务器
```

在游戏里测试可以多开实例：设置 `PBZ_ROOT` 为游戏目录，然后运行 `tools\launch.ps1 <标签>`。每个标签使用独立的 `platform\steam\<标签>\` Mod 目录，窗口不抢焦点，默认静音。`tools\stop.ps1` 用来关掉它们。

## 写一个 Mod

Mod 就是一个 DLL，导出 `PbzInit(const PbzHost*)`（可选 `PbzUnload()` 支持热卸载）。链接 `framework/` 后，对象、字段、函数都按名字访问，不用写内存地址：

```cpp
#include "uex.h"
static void OnKey(void*) {
    if (auto me = uex::Me())  // 本地玩家
        uex::Log(L"hp %.0f / %.0f", uex::Attr(me, uex::attr::Health), uex::Attr(me, uex::attr::HealthMax));
}
extern "C" __declspec(dllexport) void PbzInit(const PbzHost* h) {
    uex::SetHost(h); uex::SetLogName(L"hello");
    h->addHotkey(VK_F10, kPbzHotCtrl, OnKey, nullptr);
}
extern "C" __declspec(dllexport) void PbzUnload() { /* 撤销自己生成的东西；tick/hook/热键由加载器清理 */ }
```

字段用 `uex::Prop{L"Class /Script/Engine.Character", L"Mesh", 0x330}` 声明：第一次用时按名字查，游戏更新挪了位置也能找到，数字只是找不到时的兜底，日志会提示。

每个 Mod 一个文件夹：`pbzmp\<id>\<id>.dll`，旁边放一个同名的 `<id>.txt`，描述名称、版本、前置、冲突、接口版本、能否联机；Mod 自己的资源和设置也放在这个文件夹里（`PbzRun(dir)` 收到的就是它）。旧的平铺方式 `pbzmp\<id>.dll` 仍然能加载。完整接口、信息文件格式和构建方法见 [sdk/README.md](sdk/README.md)。

## 许可

本仓库代码采用 MIT 许可，见 [LICENSE](LICENSE)。`third_party/minhook` 采用 BSD 2-Clause 许可。
游戏本体、游戏资源，以及 sniper 用到的第三方模型和音效，都不在本许可范围内，也不随仓库分发。
