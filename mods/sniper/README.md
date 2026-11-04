# sniper：CS 步枪

在《影之刃零》里使用 AWP、AK-47、M4A4 三把步枪，手感按 CS2 的参数还原：精度、扩散 / 压枪后坐、距离衰减、换弹与拉栓时间、开镜，以及对应音效。只在单机模式下使用（`mp=block`，启用它时不能开始联机比赛）。

## 操作

| 按键 | 作用 |
|---|---|
| F6 | 拿出 / 收起步枪（同时补满弹药） |
| 1 / 2 / 3 | AWP / AK-47 / M4A4 |
| 鼠标左键 | 开火（AK、M4 为全自动） |
| 鼠标右键 | AWP：开镜 40 → 15 → 关；AK、M4：按住机瞄 |
| R / T | 换弹 |

拿着步枪时，游戏自己对这些按键的绑定会被临时屏蔽，收起后恢复。攀爬、交互时会自动放低枪口。

## 资源（仓库不包含）

模型、贴图和音效来自 Counter-Strike，版权属于 Valve，所以本仓库**不包含**这些资源，需要你自己准备。Mod 运行时从 `pbzmp\sniper_cache\` 读取：

```
pbzmp\
  sniper.dll
  sniper.txt
  sniper_cache\
    awp.bin   awp_color.png   awp_normal.png   awp_orm.png
    ak47.bin  ak47_color.png  ak47_normal.png  ak47_orm.png
    m4a4.bin  m4a4_color.png  m4a4_normal.png  m4a4_orm.png
    snd\*.wav   （例如 awp_01.wav、ak47_01.wav、m4a1_01.wav、zoom.wav、death1.wav …）
```

- `*.bin`：网格，由 `tools/glb2bin.mjs` 把 glb 模型转换而来：
  ```
  node tools/glb2bin.mjs <meshopt_decoder.mjs> <in.glb> <out.bin>
  ```
  格式：`"AWPM"`、u32 顶点数、u32 索引数，然后是 f32 位置×3、法线×3、UV×2，最后是 u32 索引。
- `*_color / _normal / _orm.png`：基础色、法线、AO-粗糙度-金属度贴图。
- `snd\*.wav`：PCM wav，文件名（不含扩展名）就是代码里引用的音效名，见 `audio.cpp` 和 `wpn.cpp`。

缺少某个文件时，Mod 会跳过它并在日志里记录，不会崩溃。

## 源码

| 文件 | 内容 |
|---|---|
| `sniper.cpp` | 入口、状态机（开火 / 拉栓 / 换弹 / 切枪）、开镜和机瞄 |
| `wpn.cpp` | 三把枪的参数表 |
| `shoot.cpp` | 弹道、命中和伤害、弹痕和曳光 |
| `model.cpp` | 加载网格和贴图，持枪姿势 |
| `overlay.cpp/.h` | 准星、弹药 HUD、瞄准镜画面 |
| `audio.cpp` | XAudio2 播放音效 |
| `input.cpp` | 键鼠状态（仅在游戏窗口在前台时），屏蔽游戏自己的绑定 |
| `tests.cpp` | 开发用：从 `pbzmp\sniper.cmd` 读测试命令（每帧读取后删除），以及转向敌人的瞄准辅助 |

构建：在仓库根目录运行 `build.bat`，产物是 `out\sniper.dll`。
