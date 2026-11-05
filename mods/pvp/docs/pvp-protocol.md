# pbzmp 网络协议 v2

服务器 `server/pbzmp_server.py`，单个 UDP 端口（默认 27777）。整数一律小端。单个数据报 ≤ 1200 字节。

## 包头（8 字节）

| 偏移 | 类型 | 含义 |
|---|---|---|
| 0 | u8 | magic = 0xB7 |
| 1 | u8 | 协议版本 = 1 |
| 2 | u8 | type |
| 3 | u8 | slot：服务器转发 GAME 时填发送者的座位号，其余为 0 |
| 4 | u32 | token：create/join 成功后由服务器分配（非 0 随机数）；之前为 0 |

| type | 方向 | 内容 |
|---|---|---|
| 0x01 CTRL | 客户端→服务器 | 文本命令 |
| 0x02 REPLY | 服务器→客户端 | CTRL 的回复 |
| 0x03 STATE | 服务器→客户端 | 房间完整状态 |
| 0x10 GAME | 客户端→服务器 | 游戏数据，服务器不解析 |
| 0x11 GAME | 服务器→客户端 | 转发的游戏数据，slot = 发送者 |

版本或 magic 不对的包直接丢弃。

## 文本格式（CTRL / REPLY / STATE）

UTF-8，字段之间一个空格。键值字段写成 `k=v`，v 做百分号编码：除 `A-Z a-z 0-9 - _ . ~` 以外的每个字节写成 `%XX`（大写十六进制）。

- CTRL：`<req> <cmd> [k=v ...]`。req 是客户端自增的 u32 十进制，从 1 开始；req=0 表示不要回复。
  客户端每 250 ms 重发同一个 req，直到收到同 req 的 REPLY，最多 20 次（5 s），之后视为超时。
  服务器按（成员 token, req）去重：处理过的 req 只重发缓存的回复，不再执行。create/join 还没有 token，按（地址, req）去重。
- REPLY：`<req> ok [k=v ...]` 或 `<req> err reason=<r> [msg=<文字>]`。

### 命令

| cmd | 参数 | 成功回复字段 | 失败 reason |
|---|---|---|---|
| create | name, ver, max（2–上限，默认 2），[pw], [pad] | code, slot, token | version, password, value, rate, server_full |
| join | code, name, ver, [pw], [pad] | code, slot, token | version, password, value, rate, noroom, full, started |
| leave | | | |
| set | ready（0/1）、weapon（资源路径）、edge（杀器 ItemId）、mods（启用的 Mod 列表），可只给其中一部分 | | phase |
| mods | slot | slot, mods（该成员的完整 Mod 列表） | value（没有这个成员） |
| conf | rounds（1/3/5/7/9）、time（0 或 30–600 秒）、dmg（10–500，百分比）、max（2–上限）、map（0–15，地图序号）、maps（0–65535，随机地图位掩码）、mode（duel/ffa/kill）、respawn（1–10 秒）、kills（0 或 1–50）、rules（见“房间规则”），可只给部分 | | nothost, phase, value |
| start | | | nothost, phase, players, notready |
| arena | round | | |
| dead | round, by（击败我的 slot，未知或自己为 -1），[life]（死时的命数，默认当前） | | |
| hp | round, pct（0–1000，千分比） | 无（用 req=0 发送） | |
| ping | t（客户端毫秒时钟） | t（原样返回） | |

- ver 当前为 `2`（v1.5 起；v1.4 及以前是 `1`），不一致回 `version`。包头版本号仍是 1。
- pw：服务器用 `--password` 启动时必须给出且一致，否则回 `password`。服务器没设密码时忽略 pw。客户端 API 是 `net::SetPassword()`。
- pad：只能由 `-` 组成，服务器忽略内容。create/join 整个数据报（含包头）不足 64 字节时服务器不回复（见“防放大”），客户端用 pad 补到 64 字节。
- name 最长 24 个字符，超出截断，含控制字符回 `value`；weapon、edge、mods 各最长 200 字节，含控制字符回 `value`。
- 解析是严格的：CTRL 正文只能是 ASCII（非 ASCII 字节必须百分号编码），键只能是小写字母、不得重复、最多 16 个、必须是该命令认识的键；`%` 后必须是两位十六进制，解码后必须是合法 UTF-8；整数字段只能是十进制数字（可带 `-`），还要在范围内（round 0–99，by -1–7，life 0–9999，slot 0–7，pct 0–1000，ping 的 t 是 u32）。不合格回 `err reason=bad`（req 解析不出则不回）。
- set/conf 只在 lobby 阶段有效，否则回 `phase`。conf 和 start 只有房主能发。
- start 的条件：人数 ≥ 2，且除房主外所有人 ready（房主视为已准备）。
- 带 token 的任何包（包括 GAME）都会刷新该成员的最后活动时间和地址，NAT 换了端口也能接上。token 不认识的包直接丢弃，不回复。token=0 只接受 create/join，GAME 必须带有效 token。客户端发的包头 slot 必须为 0。
- 地址验证：create/join 成功后，服务器要先收到一个带新 token 的包（通常是客户端的第一个 ping），才给这个成员发 STATE 和转发 GAME。5 s 内没收到就把他移出房间。这样伪造源地址的 create/join 拿不到 STATE 流量。
- 客户端每秒发一次 `ping`（req≠0），用来测服务器往返时间并作心跳。

## STATE

房间状态一变，服务器立刻发给所有成员；另外每收到一个成员的 ping，就给它回一份（相当于 1 s 一次的心跳）。

```
v=<版本> you=<slot> code=<房间码> phase=<阶段> round=<r> rounds=<n> time=<s> dmg=<pct> map=<i> max=<n>
host=<slot> left=<本阶段剩余毫秒，0=不限时> winner=<slot|-1> mode=<duel|ffa|kill> maps=<位掩码>
respawn=<s> kills=<n> cap=<服务器人数上限> rules=<房间规则> [msg=<通知>] m<slot>=<子字段> ...
```

（实际是一行。）

- v：房间状态版本，每次变化加 1。客户端对同一房间只接受更大的 v。
- STATE 的值（v2 起）只转义这些字节：0x00–0x20、`%`、`,`、`=`、0x7F，写成 `%XX`；其他字节（包括 UTF-8 多字节字符）原样发送。客户端按 `%XX` 解码，其余字节照抄。CTRL 仍然要求全部百分号编码成 ASCII。这样 8 人、24 字中文名的 STATE 也在 1200 字节以内。
- v2 的 `m<slot>` 是 `name,ready,weapon,score,alive,conn,edge,modh,life,deaths` 十个子字段：
  - modh：该成员 mods 文本的 FNV-1a 32 位哈希，8 位小写十六进制；mods 为空时为空。客户端发现哈希和缓存的不一样时，用 `mods slot=<n>` 取完整列表（回复 `ok slot=<n> mods=<列表>`），再填进 Member::mods。
  - life：本回合第几条命，进入 load 时清零，击杀赛每复活一次加 1。
  - deaths：本场比赛死亡次数（击杀赛用），start 时清零。
  - score：对决 / 混战是胜场，击杀赛是击杀数。
- 以下是 v1 的旧格式说明：`m<slot>` 的值是 `name,ready,weapon,score,alive,conn,edge,mods` 八个子字段（旧服务器没有第 7、8 个，客户端按空值处理；旧服务器对 set 的 edge / mods 键回 `bad`，所以 mods 单独发一条 set）。mods 是 `id:版本:标记` 用 `;` 连起来，标记 `a` = 可联机，`b` = 信息文件写了 `mp=block`；新客户端在房间里会先卸载 `mp=block` 的 Mod、不上报它们；房间里任何人有 `b` 时客户端不让房主开始，版本不同只提示，每个先各自做百分号编码，再用逗号连起来（子字段里的逗号因此会被编码），整体不再编码。ready/alive/conn 为 0/1。
- you 按接收者填写。

## 阶段

| phase | 含义 | 离开条件 |
|---|---|---|
| lobby | 等人、选武器、准备、房主改设置 | 房主 start → load |
| load | 各自进竞技场、回满状态、站到出生点 | 所有 conn=1 的成员都发了 `arena round=<当前回合>`，或 60 s 超时 → count |
| count | 倒计时 3 s，客户端不允许攻击 | 3 s → fight |
| fight | 战斗，限时 time 秒（0 不限） | 存活 ≤ 1 人，或限时到 → end |
| end | 显示本回合结果 4 s | 有人胜场达到 rounds/2+1，或已打满 rounds 回合 → over；否则 round+1 → load |
| over | 显示比赛结果 8 s | → lobby |

- lobby 阶段 round=0；start 时 round=1，所有人 score 清零。进入 load 时所有人 alive=1。
- dead 只在 fight 阶段、round 等于当前回合时有效，把发送者标为 alive=0，重复的忽略。
  存活剩 1 人：他是本回合 winner，score+1。存活剩 0 人（几乎同时死）：winner=-1，平局，不加分。
- 限时到：存活者里 hp 千分比最高的赢，并列算平局。服务器用每人最近一次 hp 报告，没报告过的按 1000 算。
- 平局回合也算打了一个回合。打满 rounds 回合时 score 最高的人赢比赛，并列 winner=-1。
- end 阶段的 winner 是本回合胜者，over 阶段的 winner 是比赛胜者，其他阶段为 -1。
- load 到 over 结束算“比赛中”，这期间 join 回 `started`。回到 lobby 时所有人 ready=0。

## 模式（v2）

conf 的 `mode`：

| mode | 人数 | 回合 | 计分 |
|---|---|---|---|
| duel（默认） | max 必须是 2 | 同 v1 | 回合胜者 score+1 |
| ffa（混战） | 2–上限 | 同 duel：存活 ≤ 1 人回合结束，限时到时存活者里 hp 最高的赢 | 回合胜者 score+1，先到 rounds/2+1 赢比赛 |
| kill（击杀赛） | 2–上限 | 整场只有 1 个回合（round=1），rounds 不用 | score = 击杀数 |

- conf 一次给的字段一起生效，生效后再检查：duel 时 max≠2 回 `value`（conf 只给 `mode=duel` 时服务器把 max 设成 2，房间里人多于 2 回 `value`）；任何 slot ≥ max 回 `value`。
- 击杀赛：
  - dead 有效时（fight 阶段、round 对、发送者 alive、给了 life 时 life 等于当前命数）：发送者 alive=0、deaths+1；by 是房间里的另一个成员时，那个成员 score+1；by=-1 或自己（摔死、自杀）不给任何人加分。
  - 有人 score 达到 kills（kills>0）：本回合结束，winner = 他。
  - 死后 respawn 秒，服务器把他 alive 置 1、life+1、hp 回 1000，发 STATE。客户端看到自己的 life 变大就在随机出生点复活，并有 1.5 s 无敌（这段时间收到的 hit 忽略）。
  - 存活人数不会结束回合。限时到：score 最高的人赢，并列 winner=-1。time=0 且 kills=0 时 start 回 `value`。
  - end 之后直接 over，winner 同 end。
- maps：随机地图位掩码，bit i 对应地图 i。非 0 时服务器在每次进入 load 时从里面随机选一张写进 map（只有一位时就是那张）；0 时用 map。
- 进入 load：所有人 alive=1、life=0。start：所有人 score=0、deaths=0。

## 房间规则（rules）

conf 的 `rules` 是 `键:值` 用逗号连起来，只能用 `a-z 0-9 : ,`，最长 200 字节，不合格回 `value`。服务器不解释，原样放进 STATE；客户端不认识的键忽略，缺的键用默认值。所有值都是非负十进制整数。

| 键 | 含义 | 范围 | 默认 |
|---|---|---|---|
| hp | 最大生命倍率（%） | 50–500 | 300 |
| sd | 杀器（杀气技能）伤害倍率（%） | 0–300 | 100 |
| sr | 杀气回复倍率（%） | 0–300 | 100 |
| ult | 大招充能倍率（%） | 0–300 | 100 |
| heal | 回血道具：0 全禁，1 只留酒葫芦，2 不限 | 0–2 | 2 |
| cimu | 1 = 禁用慈母护符 | 0–1 | 0 |
| acc | 饰品数量上限，9 = 不限 | 0–9 | 9 |
| edge | 0 = 禁用杀器 | 0–1 | 1 |
| wban | 禁用武器位掩码，bit i = fight::Weapons() 第 i+1 项（第 0 项「当前装备」不能禁） | 0–2^30 | 0 |
| wrand | 1 = 每回合随机武器（各客户端用房间码、回合、slot 算出同一个结果，跳过 wban） | 0–1 | 0 |
| stun | 连招保护：同一串连击第 N 下以后不再播受击硬直，0 = 关 | 0 或 3–10 | 6 |
| cdmg | 1 = 连段伤害递减：同一串第 4 下起每下 -10%，最低 40% | 0–1 | 1 |

「同一串」：被打方本地判定，两次命中间隔 < 0.8 s。伤害倍率仍是 conf 的 dmg。

## 掉线与房间生命周期

- 成员 5 s 没有任何包：conn=0。15 s 没包：移出房间；比赛中（load 到 over）放宽到 120 s，留给崩溃后重启游戏重连。客户端每秒 ping，正常不会触发。
- 带着已知 token 的任何包都算这个成员回来了（conn=1）。崩溃重进的客户端用旧 token 发 ping，回复 ok 就继续，回复失败就说明座位已经没了。
- 成员 leave 或被移出后，如果在比赛中且只剩 1 个成员：直接 over，winner=剩下的人。
- 房主离开：剩下成员里 slot 最小的成为房主。房间没人就删除。
- 房间码是 6 位数字，随机生成，不与现有房间重复。slot 是 0..max-1 里最小的空位。

## GAME 转发

- 服务器把 GAME 包原样转发给同房间其他 conn=1 的成员：type 改为 0x11，slot 填发送者，token 填 0。任何阶段都转发。
- 负载最多 1192 字节（1200 减包头）。每个成员每秒最多转发 200 个包、128 KB，超出的丢弃。
- 房间超过 4 人时客户端把状态发送降到 30 Hz（见 pvp-netcode.md），8 人房间服务器上行约 3 Mbps。

## 服务器限制

数值是 `Limits` 类的默认值（`server/pbzmp_server.py`），窗口都是固定窗口。

| 限制 | 默认 | 超出时 |
|---|---|---|
| 房间总数 | 1000 | create 回 `server_full` |
| 同一 IP 同时拥有的房间（按创建者 IP） | 4 | create 回 `rate` |
| 同一 IP 每分钟 create | 6 | 回 `rate` |
| 同一 IP 每分钟 join | 20 | 回 `rate` |
| 同一 IP 10 分钟内 join 不存在的房间码 | 10 | 之后 10 分钟内这个 IP 的所有 join 都回 `rate`，房间码对也一样，所以无法靠回复猜码 |
| 同一 IP 每秒收包（任何类型） | 1000 | 丢弃 |
| 同一地址（IP+端口）每秒 CTRL | 20 | 丢弃 |
| 同一 IP 每分钟 token 不认识的 CTRL | 30 | 丢弃 |
| 每个成员每秒转发 GAME | 200 包、128 KB | 丢弃 |
| 每秒发往同一 IP 的字节 | 512 KB | 丢弃 |

- 房间码和 token 用系统加密随机数（`secrets`）生成。
- 过期清理：房间状态 1 小时没有任何变化就整个删除（正常房间每次 join/set/阶段变化都会刷新；没人 ping 的成员 15 s 就会被移出）。限流表定期清理。

### 防放大

- 对没有有效 token 的包（token=0，或 token 不认识时重发的 leave 回复），服务器最多回一个数据报，且不比请求大；会更大就不回。
- token=0 的数据报不足 64 字节时直接丢弃，客户端用 `pad=` 补齐（见上）。
- STATE 和转发的 GAME 只发给已经过地址验证的成员（见“地址验证”）。
- 发往同一 IP 的流量有字节上限，限制有人用自己的 token 从伪造地址把转发流量引向别人。

## 运维

```
python3 -I pbzmp_server.py [--port 27777] [--bind 0.0.0.0] [--password 口令] [--log-file 路径] [--quiet]
```

- `--log-file`：日志追加写入该文件（UTF-8），stdout 照常输出；`--quiet` 关掉 stdout。
- 每 60 s 输出一行 `status rooms=<n> players=<n> pps=<收包/秒> ips=<n>`。
- Ctrl+C（SIGINT）、SIGTERM，以及 Windows 下的 Ctrl+Break 都会干净退出：跑完当前这一轮循环，记一行 `shutting down`，关闭 socket，退出码 0。
- `--loss/--delay/--jitter` 只用于测试。
- `--max-players N`（2–8，默认 8）：房间人数上限，STATE 里是 `cap`。create/conf 的 max 超过它回 `value`。
- `--egress-kb N`：每秒发往同一 IP 的字节上限（KB，默认 512，0 = 不限）。本机多实例测试 8 人时用 0。

### 部署到 Linux VPS

只需要 Python 3.8+，没有其他依赖。

```
sudo useradd --system --no-create-home --shell /usr/sbin/nologin pbzmp
sudo mkdir -p /opt/pbzmp /var/log/pbzmp
sudo cp server/pbzmp_server.py /opt/pbzmp/
sudo chown pbzmp: /var/log/pbzmp
```

`/etc/systemd/system/pbzmp.service`：

```
[Unit]
Description=pbzmp PvP relay
After=network-online.target
Wants=network-online.target

[Service]
User=pbzmp
ExecStart=/usr/bin/python3 -I /opt/pbzmp/pbzmp_server.py --port 27777 --quiet --log-file /var/log/pbzmp/server.log
# with a password: append  --password <口令>  (or put it in an EnvironmentFile and use $PBZMP_PW)
Restart=on-failure
RestartSec=2
KillSignal=SIGINT
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true
ReadWritePaths=/var/log/pbzmp

[Install]
WantedBy=multi-user.target
```

```
sudo systemctl daemon-reload
sudo systemctl enable --now pbzmp
journalctl -u pbzmp -f                 # 或 tail -f /var/log/pbzmp/server.log
```

防火墙只放行 27777/udp：

```
sudo ufw allow 27777/udp               # ufw
sudo firewall-cmd --permanent --add-port=27777/udp && sudo firewall-cmd --reload   # firewalld
```

云服务商的安全组也要放行 27777/udp 入站。日志会一直增长，可用 logrotate 配 `copytruncate`。

## 战斗事件（GAME 事件文本，客户端之间）

服务器只转发，不解析。字段用空格分隔，`round` 不是当前回合的事件会被丢弃。

| 事件 | 通道 | 含义 |
|---|---|---|
| `hit <victim> <amt> <attacker> <frac> <round> <posture> <yaw>` | SURE | 攻击方打中了 victim 的幽灵。`amt` 是已按 dmg% 缩放的伤害，`frac` 是它占 victim 最大生命的比例（大于等于 1.5 表示致命），`posture` 是架势伤害占比，`yaw` 是攻击方当时的朝向（度）。`posture` 和 `yaw` 可以省略，旧版客户端只发 5 到 6 个字段。 |
| `res <attacker> <victim> dodge\|pdodge\|block <round>` | SURE | victim 在自己这边判定这次 hit：闪避无敌帧（`dodge`，完美闪避为 `pdodge`）不扣血；`block`（按住弹反）只扣架势。攻击方收到后停掉幽灵身上本地播放的受击动作；`block` 时攻击方自己中断攻击，播放当前武器的 `AM_Hun_<武器>_HD_AttackerFailed` 弹刀硬直（没有则 `AM_Hun_Attack_Failed`）。 |
| `W <weapon config path>` | SURE / QUICK | 当前武器（变化时 SURE，之后每 2 秒 QUICK）。 |
| `E <phantom edge config path>` | SURE / QUICK | 当前杀器（Weapon.Slot.Shachi 的配置，节奏同 W）；接收方只加载杀器配置，其他路径丢弃。 |
| `O <outfit ItemId> <skin> <hat>` | SURE / QUICK | 当前外观（DT_PBZAppearanceTable 的 type 4 行 ItemId，skin 为 EPBZSkinColor 0 到 3，hat 1 表示显示帽子；节奏同 W）。接收方只给幽灵换外观，不存档；未知 ItemId 丢弃。DLC 外观只在发送方账号拥有时才会发出，否则发送方发自己当前的外观。 |

死亡没有单独的事件：STATE 的 flags 带 16（dead），死亡蒙太奇（AM_Hun_Dead_F/B）通过 STATE 的蒙太奇字段同步，幽灵停在最后的姿势。

## 版本

协议改动不兼容时 ver 加 1（v2：create/join 的 ver=2，包头版本号保持 1，所以旧客户端能收到 `err reason=version` 并提示版本不匹配）。
