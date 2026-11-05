# 影之刃零 Mod SDK

基于 pbzmp 加载器（`version.dll`）和 Mod管理器开发自己的 Mod。

## 目录

- `pbz_host.h`：加载器提供给 Mod 的接口
- `framework/`：UE 反射框架（`uex.h`）。按名字找对象、字段、函数，不写内存地址
- `hello/`：用框架写的示例，Ctrl+F10 把血量、架势、位置写进日志（推荐从这里开始）
- `example/`：只用 `pbz_host.h` 的最小示例，每帧回调、快捷键、函数钩子、卸载

## 游戏更新后不用改代码

游戏每次更新，函数和全局变量的内存地址、类里字段的偏移都可能变。这套 SDK 里 Mod 不需要知道任何地址：

| 要找的东西 | 谁来找 | 怎么找 |
|---|---|---|
| 对象数组、名字池、ProcessEvent、引擎 Tick | 加载器 | 每次启动扫描 PBZ.exe 的代码特征，结果放进 `PbzHost`（v3），写进 `loader.log` |
| 类、对象、函数 | 框架 | 按全名在对象数组里查（`uex::Class`、`uex::Fn`），带缓存 |
| 字段偏移 | 框架 | 按名字读 UE 反射信息（`uex::Prop`、`uex::Field`），带缓存 |
| 函数参数 | 框架 | 按参数名读写（`ue::Params`） |

`uex::Prop` 里写的数字只是兜底：字段被改名或删了才用它，并在日志里写一行 `field X.Y not found`；字段只是挪了位置会写 `field X.Y moved: 0x.. (was 0x..)`，Mod 照常工作。更新后看一眼 `loader.log` 就知道有没有要处理的。

只有 UE 引擎升级（例如 5.7 到 5.8）才可能需要更新 `framework/ue.h` 里的结构布局，这由框架维护，Mod 不用管。

## 一个 Mod 是什么

一个 x64 DLL，和它的信息文件、自带的资源一起放在自己的文件夹 `<游戏>\platform\steam\pbzmp\<id>\` 里：

```
pbzmp\
  autorun.txt  loader.log  cmd.txt     加载器的文件（根目录）
  <id>\
    <id>.dll
    <id>.txt
    ...                                你的 Mod 自己的文件（资源、设置）
```

`PbzRun(const wchar_t* dir)` 收到的 `dir` 就是这个文件夹（`pbzmp\<id>\`，以 `\` 结尾），Mod 自己的文件都相对它读写。`host->dir` 是根目录 `pbzmp\`，只放加载器的文件。

旧的平铺方式 `pbzmp\<id>.dll` + `pbzmp\<id>.txt` 仍然能加载（这时 `dir` 是 `pbzmp\` 本身），两处都有时子文件夹优先。

`<id>` 建议带作者前缀，例如 `seed.pvp`、`zhang.camera`，避免重名。

Mod管理器会把勾选的 Mod 写进 `pbzmp\autorun.txt`，游戏启动、引擎就绪后加载器按顺序加载它们。

## 导出函数（都可选，`extern "C"`）

| 函数 | 线程 | 用途 |
|---|---|---|
| `void PbzInit(const PbzHost*)` | 加载线程 | 保存 host，注册回调、钩子、快捷键 |
| `void PbzRun(const wchar_t* dir)` | 加载线程 | 在 PbzInit 之后调用，`dir` 是 Mod 自己的文件夹 |
| `void PbzUnload()` | 游戏线程 | 卸载前撤销你对游戏做的一切 |

有 `PbzUnload` 的 Mod 可以热重载：在 `pbzmp\cmd.txt` 写一行 `run <id>` 会重新加载，写 `stop <id>` 会卸载。

## PbzHost

`host->version` 是接口版本，新字段只追加。用新字段前先检查版本。当前版本 3。

| 版本 | 字段 | 说明 |
|---|---|---|
| 1 | `dir` | 根目录 `pbzmp\`，以 `\` 结尾 |
| 1 | `engine()` | UGameEngine 指针，第一帧之前是 null |
| 1 | `addTick` / `removeTick` | 每帧在游戏线程回调 |
| 1 | `post` | 下一帧在游戏线程执行一次 |
| 1 | `log` | 写一行到 `pbzmp\loader.log` |
| 2 | `hook` / `unhook` | 通过加载器挂函数钩子。同一地址已被别的 Mod 挂了就返回 0，并在日志里写明双方 |
| 2 | `addHotkey` / `removeHotkey` | 注册快捷键（游戏窗口在前台时按下触发）。组合键已被别的 Mod 占用就返回 0 |
| 3 | `objects` / `namePool` | 对象数组、名字池，加载器启动时找到 |
| 3 | `processEvent` / `tickSlot` | UObject::ProcessEvent、UGameEngine::Tick 的虚表序号 |

回调都包在 SEH 里：每帧回调或快捷键回调崩溃时，加载器会记日志并移除它，游戏不会跟着崩。卸载时，你通过 host 注册的回调、钩子和快捷键都会被自动清掉。

请用 `host->hook` 而不是自己改游戏代码，这样加载器才能发现冲突。

## 框架 `framework/`

把 `framework\uex.cpp` 和 `framework\ue.cpp` 一起编进你的 DLL，`#include "uex.h"`。框架自己从加载器拿到 v3 的全局信息，不需要你传。只能在游戏线程用（每帧回调、快捷键回调、`post`）。

```cpp
extern "C" __declspec(dllexport) void PbzInit(const PbzHost* h) {
    uex::SetHost(h);           // uex::Log、uex::Engine 等要用
    uex::SetLogName(L"hello"); // 日志前缀
}

UObject* me = uex::Me();                                    // 本地玩家角色，菜单里是 null
float hp = uex::Attr(me, uex::attr::Health);                // 属性：Health、Shachi（架势）、Ult ...
uex::SetAttr(me, uex::attr::Shachi, 0.f);

ue::Params p(uex::Fn(me, L"GetVelocity"));                  // 按名字调 UFunction
p.Call(me);
auto v = p.Get<uex::Vec>(L"ReturnValue");                   // 参数、返回值也按名字

static uex::Prop kMove{L"Class /Script/Engine.Character", L"CharacterMovement", 0x338};
UObject* cm = kMove.Of<UObject*>(me);                       // 原生类的字段，第一次用时按名字查
float* walk = uex::Field<float>(cm, L"MaxWalkSpeed");       // 任意对象的字段（含蓝图类），没有就是 null
```

| 分类 | 主要函数 |
|---|---|
| 对象 | `Class`、`Lib`、`ue::FindObject`、`ue::NameOf`、`ue::IsA`、`Alive`、`Hold`/`Get`（安全地长期持有对象） |
| 世界和玩家 | `Engine`、`World`、`PC`、`Me`、`Loc`、`Teleport`、`Spawn`、`Destroy`、`Trace` |
| 反射 | `Fn`、`ue::Params`、`Prop`、`Field`、`OffsetOf`、`StructSize` |
| 属性 | `Attr`、`SetAttr`、`AttrSet`、`ASC`、`attr::*` |
| 界面 | `CreateWidget`、`AddToViewport`、`SetText`、`SetVisible`、`UiMode`、`GameMode` |
| 其他 | `LoadAsset`、`Console`、`Log`、`SehScope`（把访问冲突变成可捕获的异常） |

完整说明见 `framework/uex.h` 的注释。类名、字段名可以用 UE4SS 之类的工具导出 SDK 后查看。

注意：不要每帧调用返回 FString 的 UE 函数（返回值不会释放，会一直涨内存）。

## 信息文件 `<id>.txt`

UTF-8，每行一个 `key=value`：

```
name=显示名
version=1.0
author=作者
desc=一句话说明
requires=seed.pvp>=1.1,other   # 前置 Mod，可带最低版本；缺了不让保存
conflicts=zhang.camera          # 不能同时启用
api=3                           # 需要的 host 版本；加载器太旧时不加载，管理器也会提示（用了框架就写 3）
game=CL-302256_JKS-17280        # 测试过的游戏版本（PBZ.exe 的文件版本），只作提示
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
cd hello
build.bat
```

新建文件夹 `pbzmp\hello\`，把 `hello.dll` 和 `hello.txt` 复制进去，在 Mod管理器里勾选，然后启动游戏。日志在 `pbzmp\loader.log`。

## 和别的 version.dll Mod 共存

把别的 Mod 的 `version.dll` 改名为 `version_chain.dll`，放在同一目录，本加载器会顺带加载它。Mod管理器检测到时会提示，并可以一键改名。

## 注意

- 不要让 Mod 读写存档，测试时也是。
- 不要做解锁付费 DLC 的 Mod。
