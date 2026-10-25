# party（本地多人，`native/party` + `native/input`）依赖的事实与 P1 验收记录（v2）

来源标注同 `engine-facts.md`。Dev2 = `Win64_For_Mod_Dev2`，UserDir `ELMP_Test_Data\dev`，测试存档槽 0，map_church_08 椅子旁。引用 v1 结论注明 `v1 party-facts「…」`。

## P1b 输入与座位（v2 2026-10-25）

### 接线与 G0
- 设备接线（`UnrealWindow` 子类化 + 键盘 Raw Input 注册、`XINPUT1_3.dll` 序号 2 导入表项改写）**按需安装**：第一次有设备座位加入或开启静默时才装；进程启动、无座位时三项都未安装（`elmp devices` → `window=false keyboard-raw=false xinput-iat=false`）。装上之后路由器不活跃（没有设备分配给已加入座位、未静默）时，窗口过程与 `XInputGetState` 都原样直通。`[E-实测]`
- 导入表项安装时仍指向系统 `XInputGetState`（未被别的工具改过），实测改写成功；游戏在**后台**也经该导入轮询手柄 0：Dev 模拟手柄 2（归 P1）推左摇杆 → P1 `idle_turn → run → run_stop`，Y −1957 → −2582（经“P1 手柄合并”报给原版的 0 号）。`[E-实测]`
- **性能事实**：`XInputGetState` 读一个未连接的序号约 0.33–0.35 ms（Dev2 实测：座位绑定了未连接的 `xinput:1` 时 `input.pump` 每帧 P50 339 µs；1 Hz 探测三个空序号时 P99 463 µs）。因此 v2：连接探测放到独立线程（1 Hz，原子标志），游戏线程只读已连接的手柄。修复后 P1 + P2（设备）+ P3（脚本）`frame.elmp_us` P50 13 / P95 26 / P99 52 µs，`input.pump` P99 31 µs。`[E-实测]`
- 离开最后一个座位后：路由器回到不活跃（直通），设备分配保留（`xinput:0 -> P1 (assigned P2, seat absent)`），6 s 内 `g0.reflect_calls = 0`。`[E-实测]`

### 键盘归属与吞消息
- Dev2 机器枚举到 4 个键盘（PS/2 内置按完整路径、3 个 USB 按 `vid&pid` 合并）+ XInput 0（真实 Xbox 360 手柄，只读使用）。`[E-实测]`
- 把 `kbd#3` 分配给 P2 后，注入该键盘的 Right 按下 / 抬起（Dev：`elmp kbd`，记一条 Raw Input 事件 + 向游戏窗口 PostMessage WM_KEYDOWN/UP，不经 SendInput、不影响前台程序）：P2 跑动 Y −1041 → −527，两条原版键盘消息被吞（`input.keys_swallowed` +2），P1 不动。`[E-实测]`
- 归 P1 的键盘注入的消息照常交给原版（吞计数不变），但**后台窗口里 PostMessage 的键盘消息不会让 P1 移动**（UE 只处理有焦点时的键盘输入）——这是注入手段的限制，不是路由结论；P1 的键盘路径只以“未被吞”为判据。`[E-实测]`
- 一帧内按下又抬起的键（Dev `kbd … tap`）在 v2 初版会丢失（泵只看当前按住）；已加“每键盘按下锁存”（被座位读取一次后清除），修复后 `J tap` → `jump_up → jump_apex → jump_down → landing`。`[E-实测]`

### 静默 / 绑定 / 持久化
- `elmp mute 1`（`muteInput`，不持久化）：模拟手柄 1（归 P2）与 2（归 P1）推摇杆、P1 键盘按 Z，P1 / P2 位置都不变，键盘消息被吞；分配表不变；`mute 0` 后恢复。`[E-实测]`
- `elmp bind 2 jump key J`：Z 不再跳、J 跳；写入 `Saved/ELMP/ELMP/devices.json`（schema 1，`assignments` + 按动作名的 `bindings`，键名为可读标签）。重启实例后分配与 J 绑定读回，接线仍等到有座位才装。`[E-实测]`

### 座位生命周期
- `elmp join`：P2 生成在 P1 身后（v1 摆位：70 UU × 序号，4 个一轮，抬高 10），档案 = P1 共享进度（能力 / 强化物品 / 精灵等级 / 拥有遗物与槽数 / 等级与祈祷等级）+ 座位自己的默认装备（s5000 两组），恢复后 HP 400/400（P1 的 42 个最大 HP 强化物品镜像过来）；自动分配一个已连接且未分配的手柄（`xinput:0`）并保存。截图 `p1b-three-lilies-dev.png`（P1 + P2 + P3 排开）。`[E-实测]`
- 回标题（teardown）：两个座位随世界丢弃、租约释放、路由器座位清空；重新读档后**不自动重进**（按存档恢复名单需要 saveId，P1e 实现；在此之前 fail closed，避免 A 档的队伍进入 B 档），`registry verify` consistent。`[E-实测]`

## P1c-1 规则：闸门 / 过图跟随 / 规则 1·2 / 复活召回 / 椅子 / 共享进度（v2 2026-10-25）

### 钩子机制（实施决定，`[E-源码]`）
- UE4SS C++ 对**蓝图**函数的 `UObjectGlobals::RegisterHook` 走全局 `ProcessLocalScriptFunction` 回调，并在**每次脚本函数调用**时对 `Stack.Node()->GetFullName()` 建字符串查表（`deps/first/Unreal/src/UObjectGlobals.cpp:782-806`）——热路径不可接受。v2 改为自己注册一个 `ProcessLocalScriptFunction` post 回调，只比较 `Stack.Node()` 与已解析的 4 个 `UFunction*`（`PC_Base_C:OnSwitchGameMapStart / OnGameMapSwitch / OnGameMapReadyPostFade`、`WBP_FaderLoading_C:LaunchFadeIn`），回调只累加原子计数，过图状态机在帧泵里推进。回调在第一个座位加入时注册、最后一个座位离开时注销（G0）。
- 实测：4 个函数读档后全部可解析；4 次过图共 40 次命中（每次过图对 P1 + 2 名 PX 的 PC 各 3 个事件 + 淡入，按 Controller0 过滤后推进）。`party.transit` 帧阶段 P99 1 µs。`[E-实测]`

### 权限闸门（M9，GateCache）
- 每个 Lily Pawn 有 13–14 个 `PrimitiveComponent`（`K2_GetComponentsByClass`）；对 church_08 的 3 个出口 + 1 把椅子逐组件 `IgnoreActorWhenMoving(目标, true)`。缓存键 = Pawn 句柄 + 目标对象索引 / serial，失效源 = Pawn 更换（复活）、开关变化、注册表桶插入版本（流式进来的新出口 / 椅子）、世界卸载；已卸载区域的目标在版本变化时清除（修复前 church_07 显示 `targets=10`，修复后 2）。`[E-实测]`
- **travel 闸门**（规则 3，测试辅助瞬移）：P2 被放进去 church_07 的出口（Y −6230）停在出口里、地图不变；`elmp perm 2 travel on` 后移出再进 → 原版切到 church_07，P1 被原版放到新入口。`[E-实测]`

### 过图跟随（C8）
- 4 次原版 `SwitchGameMap`（P2 有权限进门、规则 1 P3 进门、规则 2 P1 进门、规则 3 P1 进门）全部 `placed: switched → released: fade-in → finished: ready`，两名座位都在淡入前落到 P1 身后（例：P1 Y 8530，P2 8600，P3 8668 —— P1 朝左，身后 = +Y）。`[E-实测]`
- 规则 1（`elmp set mapRule 1`）：两名座位的有效 travel 变为 on（逐座位值仍为 off），P3 进 church_08 出口 → 全员切图。规则 2：P1 自身碰撞忽略全部出口（`rule-2 hold=true`），P1 独自站进出口不切图；P2、P3 也进同一出口盒后该出口放开，P1 在盒内移动即触发原版切图，全员被带走。`[E-实测]`
- 未验：P1 死亡重生触发的过图（留到 P1f 对等矩阵）。

### 复活 / 召回 / 椅子
- `elmp kill 2`（测试辅助：原版 `HPComponent:DoDamage(nil, 100000)`）→ `death` 动画；约 1.6 s 后新 Pawn 在 P1 身后、HP 400/400，两组 s5000 装备保留；事件 `death → revived`。`[E-实测]`
- 召回：P2 被放到 4273 UU 外约 2 s 后回到 P1 身后（far）；P3 被放到 P1 下方 3000 后 0.8 s 内回来（fell）。`[E-实测]`
- 椅子：P2 受伤 250/400，放到椅子旁（church_08 椅子 Y −1957）按 INTERACT（虚拟手柄边沿）→ 400/400，P1 不变、地图不变、未触发原版休息；1 s 后再按 → `rest cooldown`；P3 在椅子旁 `rested`，离开后 `not at a bench`。`[E-实测]`

### 共享进度
- 加入后两名座位 1 s 内 `synced`；测试辅助给 P2 的背包 `AddCurrency(0, 25)` → 下一拍 P1 货币 11764 → 11789，日志一笔 `P2 -> P1: currency 25`，6 s 后不变（无回声）。快照全部直读（`Items` 行、`Currencies`、`UnlockedAptitudes`、`SpiritLevels`、经验 + 3 个 getter），两名座位的 1 s 节拍约 115 µs。未验：经验跨级换算、物品行 / 精灵等级收益（P1 已满级 / 满收集，留到 P1f 用低进度存档验证）。`[E-实测]`

### 性能（P1 + P2 + P3 脚本座位，church_08 静止，15 s）
- `frame.elmp_us` P50 9 / P95 30 / P99 124 µs（门槛 300）；`party.patrol` 每 250 ms P99 42 µs；`party.progress` 每秒 P99 156 µs。两人离开后回调注销，6 s 内 `g0.reflect_calls = 0`，`registry verify` consistent。`[E-实测]`

### 测试辅助（P1c-1，登记）
- 读档 ×3；`elmp tp`（座位与 P1 的瞬移，约 15 次）；`elmp kill 2`（致死 ×1、150 伤害 ×1）；P2 背包 `AddCurrency(0,25)` ×1（经 UEBridge eval）；原版切图 4 次（church_08 ↔ 07）；`mapRule` / `recall` 设置临时修改后已恢复为 3 / 1。

### 测试辅助（P1b，登记）
- 读档 ×4、回标题 ×1；Dev 模拟手柄 `fakepad 1/2`（各数次）、键盘注入 `kbd`（kbd#1 / kbd#3，约 10 次）；只写 Dev2 的 `devices.json` 与测试世界；未向用户正在使用的程序发输入（PostMessage 只投到 Dev2 的游戏窗口），真实手柄只读。
