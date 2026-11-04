# 影之刃零 Mod SDK

基于 pbzmp 加载器（`version.dll`）和 Mod管理器开发自己的 Mod。

## 目录

- `pbz_host.h`：加载器提供给 Mod 的接口（唯一需要的头文件）
- `example/`：最小示例，每帧回调、Ctrl+F9 快捷键、函数钩子、卸载

## 一个 Mod 是什么

一个 x64 DLL，放在 `<游戏>\platform\steam\pbzmp\` 下：

- `pbzmp\<id>.dll` + `pbzmp\<id>.txt`，或
- `pbzmp\<id>\<id>.dll` + `pbzmp\<id>\<id>.txt`（自带资源时用子文件夹）

`<id>` 建议带作者前缀，例如 `seed.pvp`、`zhang.camera`，避免重名。

Mod管理器会把勾选的 Mod 写进 `pbzmp\autorun.txt`，游戏启动、引擎就绪后加载器按顺序加载它们。

## 导出函数（都可选，`extern "C"`）

| 函数 | 线程 | 用途 |
|---|---|---|
| `void PbzInit(const PbzHost*)` | 加载线程 | 保存 host，注册回调、钩子、快捷键 |
| `void PbzRun(const wchar_t* dir)` | 加载线程 | 旧接口，可不用 |
| `void PbzUnload()` | 游戏线程 | 卸载前撤销你对游戏做的一切 |

有 `PbzUnload` 的 Mod 可以热重载：在 `pbzmp\cmd.txt` 写一行 `run <id>` 会重新加载，写 `stop <id>` 会卸载。

## PbzHost

`host->version` 是接口版本，新字段只追加。用新字段前先检查版本。

| 版本 | 字段 | 说明 |
|---|---|---|
| 1 | `dir` | Mod 文件夹，以 `\` 结尾 |
| 1 | `engine()` | UGameEngine 指针 |
| 1 | `addTick` / `removeTick` | 每帧在游戏线程回调 |
| 1 | `post` | 下一帧在游戏线程执行一次 |
| 1 | `log` | 写一行到 `pbzmp\loader.log` |
| 2 | `hook` / `unhook` | 通过加载器挂函数钩子。同一地址已被别的 Mod 挂了就返回 0，并在日志里写明双方 |
| 2 | `addHotkey` / `removeHotkey` | 注册快捷键（游戏窗口在前台时按下触发）。组合键已被别的 Mod 占用就返回 0 |

回调都包在 SEH 里：每帧回调或快捷键回调崩溃时，加载器会记日志并移除它，游戏不会跟着崩。卸载时，你通过 host 注册的回调、钩子和快捷键都会被自动清掉。

请用 `host->hook` 而不是自己改游戏代码，这样加载器才能发现冲突。

## 信息文件 `<id>.txt`

UTF-8，每行一个 `key=value`：

```
name=显示名
version=1.0
author=作者
desc=一句话说明
requires=seed.pvp>=1.1,other   # 前置 Mod，可带最低版本；缺了不让保存
conflicts=zhang.camera          # 不能同时启用
api=2                           # 需要的 host 版本；加载器太旧时不加载，管理器也会提示
game=CL-302256_JKS-17280        # 适配的游戏版本（PBZ.exe 的文件版本），游戏更新后用来判断是否需要重新适配
mp=allow                        # allow：能带进联机；block：房间里有人启用时不能开始对战（修改器、作弊类请写 block）
hidden=1                        # 框架类 Mod，不在列表里显示
```

## 联机

进入联机房间后，每个人启用的 Mod 列表（id、版本、是否 block）都会发给房间里的其他人：

- 版本不同时只提示；
- `mp=block` 的 Mod 在进联机房间时被自动卸载（加载器 `stop`），离开房间后重新加载（`run`），所以它必须导出 `PbzUnload` 并能干净卸载。房间里仍有人上报这类 Mod 时，房主不能开始对战。

## 构建

需要 Visual Studio 2022（Build Tools 即可），目标 x64。

```
cd example
build.bat
```

把 `example.dll` 和 `example.txt` 复制到 `pbzmp\`，在 Mod管理器里勾选，然后启动游戏。日志在 `pbzmp\loader.log`。

## 和别的 version.dll Mod 共存

把别的 Mod 的 `version.dll` 改名为 `version_chain.dll`，放在同一目录，本加载器会顺带加载它。Mod管理器检测到时会提示，并可以一键改名。

## 注意

- 不要让 Mod 读写存档，测试时也是。
- 不要做解锁付费 DLC 的 Mod。
