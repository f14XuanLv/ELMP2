# ELMP v2 开发规则（新仓库）

- 需求以 `docs/REQUIREMENTS-TREE.md` 为准，架构以 `docs/ARCHITECTURE-V2.md` 为准；新决定先改文档再改代码。
- Shell 命令前缀：`[Console]::OutputEncoding = [System.Text.Encoding]::UTF8;`；Python 用 `C:\Dev\miniconda\python.exe`。
- UE4SS API 以 `D:\CodeT\RE-UE4SS-32d8a381`（源码与 docs）为准；引用方式：submodule + `add_subdirectory` + 链接 `UE4SS` 目标（同 `D:\CodeT\Lilac2LilyMod`）。

## 用户在用电脑时：不做前台类自动化（用户要求，2026-10-26）
- 不恢复 / 移动 / 激活实例窗口（不调 `ShowWindow` / `SetWindowPos` / `SetForegroundWindow`），不向窗口投递鼠标消息，不动系统光标。
- 测 ELMP 界面一律走后台命令：`elmp manager open|close`、`elmp menu <slot> …`、Dev `elmp ui click|move <x> <y>`（直接喂覆盖层自己的事件队列，不经窗口消息）。
- 截图只在窗口本来就未最小化时做；最小化就不截，改用命令输出作证据。

## 与旧仓库 `D:\CodeT\ELMP` 的关系：拿走知识，不照搬代码
- 旧仓库的代码、`docs/REQUIREMENTS.md`、`docs/adr/`、`docs/kb/*.md`、`data/RVA.json` 是**知识来源**：原版入口、RVA / 偏移、实测结论、踩过的坑、协议语义。实现前先去读。
- **不复制旧仓库的代码文件**（含 C++、Lua、测试、mock）；每个模块按 v2 架构重新实现。可以复用的只有：原版事实、数值、RVA 与前缀、`data/` 下由工具从游戏包生成的素材（图标 / 字体 / 文本表，由新仓库的工具重新生成）。
- kb 的新实测写在新仓库 `docs/kb/`，带 ELMP v2 日期；需要引用旧 kb 结论时注明出处。

## 资料与知识来源（只读；用途与可信度）
| 路径 | 是什么 | 怎么用 |
|---|---|---|
| `D:\CodeT\ELMP` | v1 工程（C++ 机制层 + Lua 逻辑层）、`docs/REQUIREMENTS.md`、`docs/adr/`、`docs/kb/*.md`、`data/RVA.json` | 原版入口 / RVA 与前缀 / 实测结论 / 协议语义 / 踩坑记录的第一来源；**不复制代码** |
| `D:\CodeT\EnderLilies` | 由 Shipping 反射重建的 UE 工程：`Source/Zenith/Public|Private`（551 个头 + 386 个 cpp，头里是 UPROPERTY/UFUNCTION 声明，cpp 只有构造函数默认值与空实现）、`Plugins/`（**SpinePlugin / SpineExtensionPlugin / EventPlugin / FMODStudio 等源码**）、`Config/` | 类成员、函数签名、枚举值、CDO 默认值；Spine 组件的 C++ 布局（`UTrackEntry::entry`、`USpineSkeletonAnimationComponent::state`、`USpineSkeletonComponent::skeleton`）；**不是实现**，行为要看 IDA / CUE4Parse |
| `C:\Code2\UnrealEngine-4.26.2-release` | 引擎源码 | 引擎语义（`UpdateOverlapsImpl`、`FWeakObjectPtr`、`ULevelStreamingDynamic`、`ProcessEvent`、`FUObjectArray`）；语义不确定就 grep，不猜 |
| `D:\CodeT\RE-UE4SS-32d8a381` | UE4SS 源码与 docs | C++ Mod API（`Hooks.hpp`、`CppUserModBase.hpp`、`UObjectGlobals.hpp`、`UObjectArray.hpp`、`LuaMod.cpp`）；行为以源码为准 |
| `D:\CodeT\LilyInEM` | 同作者的纯 C++ UE4SS Mod（EL 作 Guest 嵌进 EM）：`native/rt/ue.h`（缓存化反射 + 按 FProperty 偏移布参数的 `Call`）、`native/core/{seqlock,spsc_ring,pose_ring,binary_manifest,protocol}.h`、`rt/{hook,d3d,input}`、`data/binary/EL.json`（运行时加载的 SHA-256 / RVA / 前缀 / 原始偏移清单） | v2 架构里反射层、双缓冲模型、二进制清单、D3D 覆盖层的**参考实现与接口形态**；其 `AGENTS.md` 里的崩溃教训（`ForEachProperty` 遍历未知对象、空结构体参数）同样适用。复用策略同 v1：拿设计与事实，按本仓库布局重写 |
| `D:\Mod_Investigation\ELEM\EnderLilies.Randomizer` | 第三方随机化 Mod（C++，Detours，`xinput1_3.dll` 代理注入，SDK dump 头，`EnderLilies.Game/Randomizer.cpp` 钩 `BP_Interactable_Item_C::OnInteract` / `USummonerComponent::OnEquipSpirit`，按 `UClearableComponent` 做检查点，SharedMemory 对接追踪器） | 另一份“原版入口清单”的交叉验证来源（物品 / 清除点 / 存档 / 死亡 / 结局）；也是 SDK 用户的真实形态参考 |
| `D:\CodeT\Lilac2LilyMod` | UE4SS C++ Mod 引用方式、`docs/RVA_RELOCATION.md` | submodule + CMake 链接方式；游戏更新后 RVA 重定位流程 |
| `D:\Mod_Investigation\HKSS\HKMP`、`SSMP` | 空洞骑士联机 Mod | 可靠层 / 快照 / 传输接口（`IEncryptedTransport`）的设计出处 |

证据等级（写 kb 时标注）：`[E-IDA]` > `[E-实测]` ≥ `[E-CUE]` > `[E-SDK]`（重建头 / 插件源码，只证明声明与默认值）> `[E-源码]`（引擎 / UE4SS 源码，证明语义）。

## Shipping 零 Lua
- 发布包 `Mods/ELMP/` 只有 `dlls/main.dll` + `data/` + `elmp.json`；打包脚本断言 zip 内无 `.lua`。
- Dev 构建可带义肢 Lua（经 UEBridge eval 加载）与自测脚本，但 ELMP 自身逻辑不依赖任何 Lua。
- 游戏线程每帧入口是 `RegisterEngineTickPreCallback`；不要用 `on_update`（它在 UE4SS 事件循环线程）。

## 验证以真实实例为主，不为测试而测试
- 功能验收 = 在隔离实例（Dev2 / Host2 / Client2，路径只写在 `devconfig.json`）上用义肢驱动 + 截图 + `elmp` 状态命令 + 帧时间，结论写进 kb。
- 只为“游戏里难以穷举输入”的纯逻辑写少量 CTest：编解码边界、传输状态机、握手、存储原子写、预检前缀。不要给巡检 / 座位 / 过图 / 复活 / HUD 等写 mock 测试，不建 FakeEngine，不迁移旧仓库的 Lua 测试。
- 写测试前先问：这条断言在真实实例里 5 分钟内能否验证？能就不写静态测试。

## 实例与部署
- 开发实测时**可以直接删除实例里旧仓库部署的 `ue4ss/Mods/ELMP`，部署新仓库的 ELMP**；三个实例都是隔离实例，不碰正式游戏目录与正式存档。
- 实例运行中不替换 DLL（先结束实例）；只操作 `devconfig.json` 内的实例。
- 用户可能正在操作 P1：未经确认不向用户的游戏发输入；自动化测试只在隔离实例做。

## 原生 Hook 纪律（不变）
- PolyHook2；Hook 前校验 EXE SHA-256 与函数前缀；能反射取地址不写死 RVA；不 Hook `IsPlayer` 本体（`0xD150F0`）。
- “某函数没被调用”必须有原生 detour 计数或 IDA xref 证据；UE4SS UFunction 钩子看不到原生直接 / 虚表调用。
- 任何缓存必须带失效源与审计方式（`docs/ARCHITECTURE-V2.md` §16.1）。

## 常用命令（仓库根目录；Python = `C:\Dev\miniconda\python.exe`）
- 构建 + CTest：`cmd /c build.bat test`（首次配置约数分钟；离线依赖目录写在 `devconfig.local.json` 的 `fetchCache`）；只编 base + CTest：`build.bat base test`；发布包：`build.bat package`（Shipping zip 断言零 `.lua`）。
- 产物：`build\Game__Shipping__Win64\{dev,shipping,frameprobe}\dlls\main.dll`。
- 部署（实例必须已停止）：`python tools\deploy.py dev [--flavor shipping] [--elmp on|off] [--probe on|off]`；`--elmp off --probe on` = 裸 UE4SS 帧时间基线。
- 实例：`python tools\instance.py start|stop|status|log dev [--grep ELMP]`（只结束 devconfig 内实例的进程）。
- 桥接（Dev 构建，UEBridge 文件通道，不需要 MCP）：`python tools\bridge.py dev wait-ready`、`... dev elmp "elmp status"`、`... dev eval "<lua>"`、`... dev console "<cmd>"`。读档：eval `FindFirstOf('TitleMap_C'):OnSaveSlotSelected(0, 2)`（测试辅助，登记）。
- 截图：`python tools\capture.py dev <name>` → `<userDir>\Saved\ELMP\pilot\shots\<name>-dev.png`。
- 帧时间 A/B：部署探针后 `bridge.py dev console "elmpprobe start 60 <label> 2"`，结果写 `Mods\ElmpFrameProbe\out\<label>.json` 与 UE4SS.log；ELMP 自身每帧开销看 `elmp perf`（`frame.idle_elmp_us`、`g0.reflect_calls`）。测量前 `r.VSync 0` + `t.MaxFPS 60`。
- 控制台命令：`elmp status | preflight | perf [reset] | registry [verify|list <kind> [n]|menu on|off] | lifecycle | events [n] | set | get`。
- core（P1a）：`elmp spawn <slot> [baseline|default|full|max|mirror|{json}] [skin]`（101–1000 为 Dev 测试 slot，租约归 console）、`despawn <slot> [keep]`、`respawn <slot>`、`lily <slot>`、`profile <slot> [json|apply <p> [exact]|reset <p>]`、`vitals <slot> [restore]`、`skin <slot> [n]`、`pad <slot>|allow1 on|off`、`move <slot> x y`、`hold <slot> CMD[+CMD] [0]`、`press <slot> CMD`、`stop <slot>`、`trace <slot> [ms]`（Dev：记录 track-0 动画变化沿，再次不带 ms 调用读取）。时序输入用多次 bridge 调用 + `Start-Sleep`；点按后至少隔一次调用再改轴（v1 core-facts「输入」）。
- party（P1b）：`elmp join [script] [pad N] [slot N] [friendly|enemy]`、`leave [slot]`、`seats`、`devices`、`assign <pad N|kbd#N|id> <slot|p1>`、`bind <slot> <action> key|pad <label>`、`bindings <slot> [reset]`、`mute [0|1]`；Dev 测试辅助 `kbd <device> <key> down|up|tap`（只 PostMessage 到实例窗口）、`fakepad <N> off|<buttons> [lx ly]`。Dev2 机器上 `xinput:0` 是真实手柄（只读）。座位不要绑定未连接的手柄做性能测量。
- 规则（P1c）：`elmp progress`、`gates`、`perm <slot> <gate> on|off`、`rest [slot]`、`transit`；`elmp set mapRule 1|2|3`、`set recall 0|1`；Dev 测试辅助 `kill <slot> [dmg]`（原版 DoDamage）、`tp <slot> <dy> <dz>`（相对 P1，slot 1 也可）。测闸门 / 出口时先 `set recall 0`，否则 2 s 后被召回。
- 镜头 / PvP（P1c-2）：`elmp camera`（放宽状态、Ally 注册数）、`elmp team <slot> friendly|enemy`、`elmp pvp`（队伍、倍率 / 击退档、flips / scales / knocks）；规则 `elmp set pvpDamage 0-1`、`pvpKnockback 0-3`、`cameraFraming 0|1`、`cameraSideMargin|TopMargin|BottomMargin|MaxDistanceScale`。镜头读数：eval `FindFirstOf('BP_GameplayCamera_C'):GetCurrentMin()/GetCurrentMax()` + `K2_GetActorLocation()`（X = −距离）。church_08 没有敌人，怪物格去 church_07（`elmp tp 1 <dy> <dz>` 进 church_08 的 church_07 门 Y −6230 / Z 320）。
- 覆盖层（P1d）：`elmp hud`（座位数值、SkillCache、锚点与投影对账）、`elmp menu <slot> [up|down|left|right|confirm|back|context|prev|next|close]`、`elmp manager open|close`、Dev `elmp ui move|click <x> <y>`（后备缓冲像素；标签页等控件先 move 再 click）。图标 / 字体 / 物品文本重新生成：`<python> tools\catalog\extract.py`（需 .NET 10，路径来自 devconfig 的 `paks / usmap / cue4parse`）。
- 存档作用域（P1e）：`elmp storage [new|reuse]`（绑定状态、指纹、PX 记录）；`elmp gates` 列触发器策略与未登记类。触发器类清单：`<python> tools\catalog\extract.py --triggers 1`。ELMP 数据在 `<userDir>\Saved\ELMP\ELMP\{save_index.json, party_<saveId>.json}`；测 New / Reuse 时只改这些 ELMP 文件，不动 `SaveGames\*.sav`。
- 截图前确认 Dev2 窗口未被最小化（最小化时 `capture.py` 得到 1×1 的空 PNG）；最小化时不要恢复窗口（见上方“不做前台类自动化”），改用命令输出作证据。
- UE4SS 的 UFunction 钩子对蓝图函数按字符串比较每次脚本调用，热路径别用；需要蓝图事件时照 `party/transit` 的指针比较 ProcessLocalScriptFunction 回调做，并且只在有座位时注册。
- 读档后回标题再读档：`bridge.py dev eval "local gs=StaticFindObject('/Script/Engine.Default__GameplayStatics'); gs:OpenLevel(FindFirstOf('GameInstance'), FName('TitleMap'), true, ''); return 'ok'"`（测试辅助，登记）。
- 代码里避免名为 `check` / `verify` 的任何标识符（含控制台辅助函数）：UE 宏会吞掉。

## 术语
- 非 P1 的权限 / 结算说法用“权限闸门 / 结算拦截 / 存档保护”，不用“防火墙”。
- Host 权威铁律：客人进程永不自行结算；Host 做不到的场景 fail closed。
