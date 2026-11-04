# pbz-modkit

《影之刃零》（Phantom Blade Zero，PC / Steam 版）的非官方 Mod 平台：DLL 加载器 + Mod管理器 + 开发 SDK，附带一个 1v1 联机 PvP Mod。

> 非官方项目，与游戏开发商无关。仓库不含任何游戏文件或资源，不解锁付费 DLC，PvP 模式不读写存档。
> 内存偏移与当前游戏版本绑定，游戏更新后可能失效。

## 组成

| 目录 | 内容 |
|---|---|
| `loader/` | `version.dll` 代理加载器：转发系统导出，在游戏主线程每帧回调 Mod，支持热加载 / 卸载、统一的函数 Hook（MinHook）和热键接口，可串联已有的 `version.dll`（改名为 `version_chain.dll`） |
| `modman/` | Mod管理器（Win32 GUI + 命令行）：启用 / 禁用、前置与版本、冲突、接口版本检查 |
| `framework/` | UE 5.7 运行时访问（对象、反射调用、游戏线程工具），供 Mod 链接 |
| `sdk/` | Mod 开发文档和示例（`sdk/README.md`） |
| `mods/pvp/` | 联机 PvP Mod：房间大厅、武器 / 杀器 / 外观选择、擂台地图、网络同步，以及中转服务器（Python 与 C++ 两个实现） |
| `tools/` | 开发用：不抢焦点地启动多开测试实例（需设置环境变量 `PBZ_ROOT` 为游戏目录） |
| `third_party/minhook` | [MinHook](https://github.com/TsudaKageyu/minhook)（BSD 2-Clause） |

## 构建

需要 Visual Studio 2022（Build Tools 即可）x64 和 Python 3。

```bat
build.bat            :: 产物在 out\
python package.py    :: 玩家用压缩包 dist\pbz_pvp.zip
```

测试：

```bat
out\nettest.exe                                  :: 网络层链路测试 + 大厅双进程测试
python mods\pvp\server\test_server.py            :: 服务器协议测试
set PBZMP_CPP_SERVER=%CD%\out\rsrv.exe && python mods\pvp\server\test_server.py   :: 同一套测试跑 C++ 服务器
```

## 安装（玩家）

把 `pbz_pvp.zip` 解压到游戏目录的 `platform\steam\`（和 `PBZ.exe` 同一层），详见 [mods/pvp/README.md](mods/pvp/README.md)。

## 写 Mod

Mod 是一个导出 `PbzInit(const PbzHost*)` 的 DLL，放在 `pbzmp\<id>.dll` 或 `pbzmp\<id>\<id>.dll`，旁边放同名 `<id>.txt` 说明文件。接口见 `loader/host.h`，文档和示例见 [sdk/](sdk/README.md)。

## 许可

MIT，见 [LICENSE](LICENSE)。`third_party/` 下的代码遵循各自的许可。
