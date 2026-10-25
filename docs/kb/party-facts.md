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

## P1d 覆盖层：HUD / PX 装备菜单 / 管理器 / 规则页（v2 2026-10-26，Dev2）

### 数据与渲染
- `tools/catalog`（CUE4Parse + devconfig 的 usmap）重新生成 `data/catalog/items.json`（精灵 26、遗物 41：5 语言名字 / 简介、`ItemSpiritType`、`SlotCount`、`bAvailableInGame`、图标 id）、`data/icons`（68 张，含 `ui_mouse_cursor`）、`data/fonts`（FOT-PearlStd-L + chinese-traditional / korean / russian）。与 v1 数据一致（遗物槽位逐条相同）。`[E-CUE]`
- 覆盖层：探针交换链取 Present / ResizeBuffers（system 来源 detour，预检后安装），ImGui 只用 DX11 渲染后端。**不用 ImGui Win32 平台后端**：它在游戏窗口处于前台时每帧用 `GetCursorPos` 覆盖鼠标位置（imgui_impl_win32 `UpdateMouseData`），注入 / 窗口钩子给的位置会被冲掉；鼠标只来自窗口过程钩子（`input/rawinput` 的 `set_mouse_hook`），`DeltaTime` 自算。`[E-源码][E-实测]`
- 模型：`overlay/model.h` 固定尺寸结构体，游戏线程 staging → seqlock 发布，渲染线程拷出后只画；界面操作经 SPSC 环回游戏线程。渲染线程每帧约 45 µs（P50，含拷贝）。`[E-实测]`
- HUD 头顶锚点：P1 `PlayerCameraManager.CameraCachePrivate.POV`（Location / Rotation / 水平 FOV，MaintainXFOV）+ 覆盖层后备缓冲尺寸自算投影；每秒与原版 `ProjectWorldToScreen` 对账一次，实测偏差 **0.00 px**（church_08，1280×720）；连续 3 次 > 2 px 则改回原版函数。`[E-实测]`

### 实测（church_08，P1 + P2 设备座位 + P3 / P4 脚本座位）
- HUD：头顶标签 / 血条 / 祈祷点 / 椅子提示，右侧面板（敌方座位紫色），技能格图标与 ∞（`p1d-hud-dev.png`）。经菜单给 P3 装上无头骑士（s2002）后 SkillCache 重建：CastCount、剩余 12、Recast 4.5 s（与 v1 技能表一致）；`elmp press 3 SUMMON_1` 后 0.9 s 读到剩余 11、冷却 0.93，5 s 后冷却 0（`p1d-cooldown-dev.png`）。`[E-实测]`
- PX 菜单（`elmp menu 3 …`）：打开即 `IsGamePaused=true`、PX HUD 隐藏；列表 26 精灵 / 34 遗物（P1 共享），布局同 v1（`p1d-menu-list-dev.png`）；装精灵走 profile 精确应用（原版 Unequip / Equip），遗物 `EquipPassive` → 槽位 2/20；关闭恢复世界。座位死亡（`kill`）与回标题时菜单自动关闭、世界恢复运行。`[E-实测]`
- 管理器：按钮（`p1d-button-dev.png`）经覆盖层注入点击打开，暂停世界、列 8 个设备（中文设备名正常显示）；规则页切换 `equipAnywhere` 1 → 0 写进 settings.json；P2 手柄“Up”绑定经模拟手柄捕获为 Y（随后重置）。设备勾选显示**有效**归属：保存给已离开座位的设备显示在 P1 列（`p1d-manager-dev.png`、`p1d-rules-dev.png`、`p1d-bindings-dev.png`）。`[E-实测]`
- 注入点击的细节：ImGui 标签页等允许重叠的控件要求按下前一帧已悬停，测试时先 `elmp ui move` 再 `elmp ui click`；普通按钮一次 click 即可。`[E-实测]`

### 性能与 G0
- 无座位、覆盖层常驻：`g0.reflect_calls = 0`；空闲帧 ELMP P50 11–12 / P99 31–38 µs（管理器按钮判定每帧约 3 µs，全直读）。
- P1 + 3 座位（HUD 可见）：初版 `frame.elmp_us` P99 222 µs（HUD 段 P50 30 µs，主要是每座位一次 `ProjectWorldToScreen` 与 250 ms 慢节拍对齐）；改为自算投影 + 每帧最多一个座位做慢节拍后 **P50 32 / P95 89 / P99 184 µs**，HUD 段 P50 14 / P99 76 µs。
- 回标题重进（带敌方座位、菜单打开中）后 `registry verify` consistent，G0 仍为 0。

### 测试辅助（P1d，登记）
- 读档 ×5、回标题 ×1；`elmp menu`（P3 装 s2002、遗物 dmgcut_LV1，仍保留在测试世界）；`elmp press 3 SUMMON_1` ×1；`elmp kill 3` ×1；Dev `elmp ui move/click`（只进覆盖层自己的队列）约 12 次；`fakepad 1 Y` ×1；`equipAnywhere` 临时改动已恢复 0；P2 绑定已 `reset`（同时清掉了 P1b 留下的 J 跳跃测试绑定）。
- 早期两次用 `ShowWindow(SW_SHOWNOACTIVATE)` 恢复 Dev2 窗口、一次 PostMessage 鼠标点击（无效）；按用户要求此后不再做任何前台类操作（AGENTS.md）。

## P1c-2 共享镜头 / 队伍 / PvP 命中窗口（v2 2026-10-26，Dev2）

### 共享镜头（C10 / M15）
- 实现：`party/camera_sync` 每 500 ms 经 P1 的 `PlayerCameraManager.RegisteredGameplayCamera`（弱指针直读）取镜头，直读 `AllyAIControllerTargets` 核对并对缺失的座位控制器调原版 `RegisterAllyController`；离开时立即 `UnregisterAllyController`，最后一名座位离开时关放宽，空闲帧不留镜头工作（G0）。`camera/framing` 两个 manifest detour（`GameplayCameraUpdate` / `GameplayCameraTargetBounds`，SHA + 前缀校验）：≥ 2 个友方目标时本次 Update 期间 `MaxDistance × scale`、目标框加边距（竖直偏移补到反侧），返回即还原。偏移经反射：`PlayerControllerTargets` / `AllyAIControllerTargets`（16 B 数组）、`CurrentSettings.MaxDistance`、`WorldSpaceLocationOffset.Z`。`[E-实测]`
- church_08，P1 + P2 相距 70：框 Y 宽 1030 = 原版 330 + 2×350，Z 高 780 = 260 + 150 + 250 + 120（WorldSpaceLocationOffset.Z）。相距 1800：开 → 框宽 2760、距离 1380；关 → 原版框宽 2060、距离 1030。相距 4500（框被房间边界钳到 Y −5860）：关 → 距离钳在 MaxDistance 2000；开 → 2191（上限 3000），Update 后 `CurrentSettings.MaxDistance` 读回 2000（未残留）。截图 `p1c2-framing-on-dev.png`（两人同框带留白）；`p1c2-framing-off` / `p1c2-far-on/off` 拍摄时窗口最小化，是 1×1 空图，数字以命令输出为准。`[E-实测]`
- **新事实**：SwitchGameMap（church_08 → 07，流式切图）后 `BP_GameplayCamera` 是同一对象，原有 Ally 注册保留（`rebinds = 0`）；回标题重进则随世界销毁，seat 随世界丢弃，放宽在 teardown 关闭。`[E-实测]`

### 队伍与 PvP（C4 / M8）
- 实现：`core/hitrules` = slot → team 表（P1 固定友方；只能绑定自己租约内的 slot）+ 配对规则（`pvpDamage`、`pvpKnockback`）；`pvp/hitgate` 在**第一次出现异队配对时**才装 3 个 UFunction pre/post（`AbilityHitbox:OnActorOverlapBegin` / `ProcessActorEnter`、`Ability:ExecuteAbilityEffectsOnTarget`）+ manifest detour `AbilityEffectQueue`，之后全队同队即 `active=false`（直通）。配对解析：Owner → Instigator 链走到 Pawn / Controller，按控制器句柄比较（每 250 ms 从 hitrules 世代重建缓存），热路径无名字查找；击退组件取法 `K2_GetComponentsByClass` 只在档位 1..3 的异队命中里调用。`[E-实测]`
- 实测（church_08 / 07，script 座位，P3 敌方）：
  | 格 | 结果 |
  |---|---|
  | 敌 → 友（P3 一挥同时打到 P1、P2） | 两人各 400 → 362，flips / scales 增长 |
  | 倍率 0.1 vs 1.0（只打 P2） | 0.1：−38；1.0：一挥致死（≥ 400）→ 原版死亡 → ELMP 复活链 1.5 s 回满 |
  | 同队（`elmp team 3 friendly`） | 0 伤，且无异队配对 → `active=false` |
  | 击退档 2 | 第一刀即 `damage_launch_neutral → launch_down → landing → wake`，位移约 184 UU |
  | 击退档 0 | 只掉血（−38），无发射（原版韧性未打空） |
  | 还原 | 三名莉莉 `Faction` 读回全 1；`AbilityHitbox_s5000_Medium` 的 AE_Knockback `bBypassStaminaCheck=false`、数据 (300,400) 原值 |
  | 座位 → 怪物 | 敌方座 P3 砍死史莱姆（100 → 0），flips / scales 不变 |
  | 怪物 → 座位 | 史莱姆打 P2 400 → 378，flips 不变 |
  | 切图 | church_08 → 07 后命中窗口照常（calls 继续增长） |
- 未验：P1 作为攻击方打敌方座（P1 只用作受击方；P1 身份同样按 `world::p1_controller` 绑定）；精灵 AI 对座位的命中（同 v1，精灵只主动攻击 Enemies 阵营）。

### 性能与 G0（P1 + P2 + P3，church_07，放宽开 + PvP active，30 s）
- `frame.elmp_us` P50 8 / P95 27 / P99 114 µs；`party.camera` 每 500 ms P99 31 µs；`pvp.bind` 每 250 ms P99 5 µs。镜头 detour 在引擎 Update 内，未计入 `frame.elmp_us`（只做标志判断 + 两个数组的弱指针读）。
- 修正：初版在最后一名座位离开后由镜头 phase 在下一空闲帧注销 Ally（`g0.reflect_calls` +2）；改为离开时同步注销、同步关放宽后，加入 / 离开 / 回标题重进全程 `g0.reflect_calls = 0`，`registry verify` consistent。`[E-实测]`

### 测试辅助（P1c-2，登记）
- 读档 ×3、回标题 ×1；`elmp tp`（座位与 P1，约 15 次，含 P1 进 church_07 门 ×1）；`elmp vitals 2 restore` ×4（只写 P2）；`elmp set recall 0`（测试期间）、`cameraFraming`、`pvpDamage`、`pvpKnockback` 临时修改，结束后 settings.json 全部恢复默认；Dev2 窗口被最小化时用 `ShowWindow(SW_SHOWNOACTIVATE)` 恢复（只对 Dev2 窗口，不夺焦点）。

### 测试辅助（P1c-1，登记）
- 读档 ×3；`elmp tp`（座位与 P1 的瞬移，约 15 次）；`elmp kill 2`（致死 ×1、150 伤害 ×1）；P2 背包 `AddCurrency(0,25)` ×1（经 UEBridge eval）；原版切图 4 次（church_08 ↔ 07）；`mapRule` / `recall` 设置临时修改后已恢复为 3 / 1。

### 测试辅助（P1b，登记）
- 读档 ×4、回标题 ×1；Dev 模拟手柄 `fakepad 1/2`（各数次）、键盘注入 `kbd`（kbd#1 / kbd#3，约 10 次）；只写 Dev2 的 `devices.json` 与测试世界；未向用户正在使用的程序发输入（PostMessage 只投到 Dev2 的游戏窗口），真实手柄只读。
