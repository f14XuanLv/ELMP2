# 战斗 Spirit 的 Summoner / Faction 初始化上下文

ELMP v2，2026-11-05。只读 IDA、既存完整 CUE 导出、重建头及当前 v2 源码；本轮没有修改 C++ / manifest、构建、调用 UEBridge 或操作游戏。本文补齐 `player-sync-graph-v2.md` §6、§8、§11.2 留下的战斗 Spirit 上下文边，不把 `ZenithCompanionCharacter` 当作 `ZenithSpirit`。

## 1. 已证结论与实施边界

**Faction 在原版装备/按类工厂生成 Spirit 时设置；Summoner 在开始一次召唤时从 source CommandAction 的 Pawn 取得。二者是不同阶段。** 当前 Client 裸 Actor 副本没有经过前一阶段，也没有后一阶段的 source command，所以不能用调用 `Appear()` 同时补齐两者。

- `[E-IDA]` `EquipSpirit` 在 SpawnActor 后，根据装备组件 owner 的控制器类型调用 `FactionComponent.SetFaction`：玩家控制器分支是 `1`。这直接解释“Host 工厂生成的魔女 Faction=1、Client 裸 copy Faction=0”所对应的确定性构造差异；本轮未重新采样两进程数值。
- `[E-IDA]` 现成较小接缝是 **Summoner → 本 PC 的 SpiritEquip → 按 class 获取/创建 Spirit**：RVA `CE1920 → CE15B0 → CF2AA0`。它能建立工厂 Owner / Faction 和装备组件缓存，不执行 Spirit Start / Appear，也不导入 Host 装备等级或设置运行中 Summoner。
- `[E-IDA]` 已查到的 Summoner 写者 `CEC5E0` 依赖 Spirit 已经存有 source CommandAction；原生调用方只有本轮 xrefs 得到的 `CF2F00`、`CF3090` 两条开始路径。它不是 `(Spirit, Pawn)` 的独立初始化 API。
- `[E-IDA][E-CUE]` 两条开始路径都激活 Actor、运行 Appear / 原版蓝图；带命令列表的路径还立即执行 Spirit 自己的 CommandQueue。**不能把完整 Start 当作“只修上下文”接到当前 Host Act 重放后面。** 同一技能会多一个生成者，且引入动作/移动/状态副作用。

因此，目前能证明的最小改造范围是“通过所属 remote PC/Pawn 的原版工厂获得正确构造的 Spirit”，不能宣称已经恢复完整召唤上下文。若要恢复 Summoner，必须先确定 source command / 召唤阶段与唯一动作写者，再选择原版启动路径；不写 `+1416/+1440/+1448/+1456` 拼装一个看似完整的对象。

## 2. 证据位置与地址约定

本次 IDA 会话 `elmp` 已核对为工作副本 `build/toolchain-smoke/ida/EnderLiliesSteam-Win64-Shipping.exe.i64`；`server_health` 的 analysis、Hex-Rays 均 ready，imagebase `0x140000000`。下文函数地址均为 **RVA**，不是 VA。身份核对原始结果位于 `build/lily-revision/summoner-idb.json`、`summoner-health.json`。

新原生反编译 / xrefs / 入口字节位于 `build/lily-revision/summoner/`，每份 `ida-0x140*.json` 保留完整函数和指令地址；关键函数均 `truncated=false`。此目录为本机忽略产物；可追溯结论在本文固化，不把被忽略文件当作唯一知识来源。

复用的 CUE 证据为 `build/toolchain-smoke/research-skill/` 下 `CASummon_Base-{class,functions}.json`、`BP_SpiritCharacterBase-{class,functions,kismet}.json`。后者 Ubergraph 为完整 59 statements、`scriptComplete=true`，不是只看调用图边的摘要。

对照重建头位于 `D:/CodeT/EnderLilies/Source/Zenith/Public/`：`ZenithSpirit.h`、`SpiritEquipComponent.h`、`SummonerComponent.h`、`CommandAction.h`、`CommandActionSummon.h`、`CommandActionSettingsSummon.h`。头只证明声明；下述副作用以 IDA / CUE 函数体为准。旧仓库 `D:/CodeT/ELMP/docs/kb/core-facts.md` 的装备与阵营知识仅作线索，没有复制代码。IDA 自动符号含 `AHogAIController`、`ReadingListEntry` 等错误类型名；不按这些名字认定游戏类。

## 3. Faction / Owner 的两条原版生成路径

### 3.1 完整装备：SpiritEquipComponent::EquipSpirit

`[E-IDA]` exec `D8F410 → CDB900`，注册表 VA `14349B360`。该实现此前已完整导出于 `build/toolchain-smoke/research-lily/ida-140CDB900.json`，本轮重新核读相关段：

| 位置 | 已证行为 |
|---|---|
| `CDBDC4` | 初始化 `FActorSpawnParameters`。 |
| `CDBDD0–CDBDF4` | 将装备组件 owner（组件 `+160`）写入 SpawnParameters.Owner。玩家使用本人 PC 的 SpiritEquip。 |
| `CDBE4F` | SpawnActor 得到战斗 Spirit。 |
| `CDBE61–CDBE81` | owner 是 `APlayerController` 时选 faction `1`。 |
| `CDBE85–CDBEB2` 附近 | 否则检查游戏 AIController 分支，取其 faction byte；非匹配控制器保留默认 `0`。 |
| `CDBEB9` | 调 `CBC740(Spirit.FactionComponent, faction)`；组件位于 Spirit `+1224`。 |
| 其后至装备登记 / 委托 | 复制原版装备参数块到 Spirit、保存 `FSpiritData.ZenithSpirit`、命令设置与 input type，加入 SummonSets，广播装备委托。 |

这里证明 **SpawnParameters.Owner 的来源**；不保证后续附身/重绑后的 Actor.Owner 永远不变。`player-sync-graph-v2.md` §11.7 已记录这个区别，验收仍须分别读 Actor.Owner、Pawn.Controller、GetSummoner。

`[E-IDA]` `CBC740` 写 faction 后调用 `CB5D50`，再广播阵营变更。`CB5D50` 根据 faction 查 collision preset 并调用目标组件虚表方法应用。因此 `SetFaction` 不是无副作用的 byte assignment；之后仍须检查 Client 的 collision / presentation 策略，不能因 faction 正确就开放结算。

### 3.2 较小的按 class 工厂

`[E-IDA]` 本轮已闭合直接调用链：

```text
remote Lily Pawn 的 SummonerComponent
  CE1920(self, &SpiritClass)
    weak SpiritEquip +184 有效才继续
    CE15B0(SpiritEquip, &SpiritClass)
      先查当前 SummonSet 中兼容的已装备 Spirit
      再查 SpiritForSpiritClasses[class]
      未命中 → CF2AA0(SpiritEquip, &SpiritClass)
                 SpawnParameters.Owner = equip.owner
                 SpawnActor(identity transform)
                 FactionComponent.SetFaction(PC ? 1 : AI faction / 0)
               → 缓存 class → Spirit
```

`CF2AA0` 在 `CF2BA5` SpawnActor、`CF2BD9` 玩家控制器分支置 `1`、`CF2C11` 调 SetFaction。没有写 Summoner weak `+1416`、source command weak `+1440`，没有 Appear，也没有完整 `EquipSpirit` 的装备参数复制 / 装备委托。它仍会执行 Actor 构造 / BeginPlay；“没有 Start”不等于所有资产构造逻辑均无副作用。

ABI 根据原生调用点与完整函数体确认：Windows x64，`RCX=self`、`RDX=指向单个 UClass* 的地址`（`TSubclassOf<AZenithSpirit>` 的引用形态），返回 Actor 指针于 RAX。**不能把 UClass* 本身当 RDX**；三者函数开头/下游均解引用 `*a2`。本文用语义名描述 native 函数，未假定重建头存在同名 UFUNCTION。

| 接缝 RVA | self 类型 / 作用 | 本次前 16 bytes |
|---|---|---|
| `CE1920` | SummonerComponent；校验 weak 装备组件，转交获取 | `48 89 5C 24 08 57 48 83 EC 20 48 8D 99 B8 00 00` |
| `CE15B0` | SpiritEquipComponent；查当前装备组 / class cache 后创建 | `48 89 4C 24 08 55 53 57 41 57 48 8D 6C 24 C1 48` |
| `CF2AA0` | SpiritEquipComponent；仅生成及 Owner / Faction 初始化 | `48 89 5C 24 20 57 48 81 EC 90 00 00 00 48 8B 05` |

这些字节是本版二进制证据，不是已加入 manifest 的可调用授权。接入前仍需沿本仓库预检机制绑定实际 EXE SHA、prefix、ABI；优先按原版拥有关系使用 `CE1920/CE15B0`，不要孤立调用底层工厂后丢掉所属缓存。

### 3.3 工厂借用的硬约束

1. `CE15B0` 对当前装备组用 `IsA(requestedClass)` 匹配，不是 exact class 比较；可能返回已装备的子类，甚至被原版系统正在使用的对象。网络绑定必须核对实际 class、seat/PC、weak serial，不得只按名称接受。
2. `SpiritForSpiritClasses` 命中在 `CE18CC` 直接返回存储的 Actor 指针，没有 `IsValid` / PendingKill 重建分支。当前 `reset_spirits` / 行消失时的 `destroy_copy` 不能直接用于这种借用对象，否则原版缓存仍保存已销毁对象。
3. 缓存是每个装备组件的 class map；它不能自然表达同一 PC、同一 class 的多个并存 wire Spirit。当前网络 row 的 index 不能直接当原版装备 slot。若出现多行同 class 必须显式判定可一对一绑定，不能让多行轮流覆盖同一 Actor。
4. 现有 `Summoner.OnPossessed` 的 `CE8560` 会从 Pawn.Controller 取得并缓存 SpiritEquip weak `+184`。候选调用需要真实且已完成 Possess 的 **对应远端** PC/Pawn；不能取 Client P1 / player index 0 的装备组件代替。
5. `CF2AA0` 假定有效 owner/world，SpawnActor 后直接取 FactionComponent，未见 null 防御。只能在游戏线程、已加载且派生于 ZenithSpirit 的 class、有效 world/owner 下进入；加载/生成失败处理仍须设计，不能把它当通用容错 spawn。
6. 本轮尚未完整核实 `SpiritForSpiritClasses` 在 `OnGameMapSwitch` / PC EndPlay 中的清理方式。采用借用方案前必须闭合该销毁边；仅“ELMP 忘掉自己的映射”不是原版缓存清理。

## 4. Summoner 的确切写者与 source command 生命周期

`[E-IDA]` `ZenithSpirit.GetSummoner` exec `DA1E10 → CE2980` 读取 weak `Spirit+1416`。本轮找到其写者 `CEC5E0`：

| 位置 / 字段 | 行为 |
|---|---|
| `CEC5FD–CEC623` | 清 Summoner weak `+1416` 和 Summoner CommandQueue weak `+1432`。 |
| `CEC65E` | 检查 source CommandAction weak `+1440`。无有效 source 时不建立 Summoner。 |
| `CEC67C–CEC693` | 从 source 调 `D0E520`（函数体直接返回 command `+440` 的 Pawn），用 `FWeakObjectPtr::operator=` 写到 Spirit `+1416`。 |
| `CEC6E8–CEC6FC` | 从 source 取 invoked input type（getter `D0C810`，读取 command `+361`）。 |
| `CEC702–CEC8D9` | 建立 Summoner movement-mode-change 与 CommandQueue-cleared 委托，Spirit 参与召唤者的状态/结束生命周期。 |
| `CEC8DE–CEC93F` | 从已取得的 summon settings 接口结果 `+1448` 复制 end/location/offset/timeout 等设置。 |
| `CEC945–CEC9C1` | 按 cast-limit 接口结果 `+1456` 绑定 `OnSourceCommandReachedCastLimit`。 |

`[E-SDK]` `FCommandActionSettingsSummon` 声明 ResummonType、SpiritLocationStyle、LocationOffset、SpiritEndType、TimeUntilAutoEnd；`CommandActionSummon` 同时实现召唤、召唤设置、cast-limit 接口。函数体中的接口获取及设置读取与这一结构对应，不能只赋一个 Pawn 就声称上下文完整。

`[E-IDA]` 配对解绑函数 `CF6EB0` 移除上述旧 source / Pawn / CommandQueue 的委托，清 source weak `+1440` 和两个接口结果 `+1448/+1456`。它不立即清所有旧 Summoner weak；下一次 `CEC5E0` 会重置。因此池复用 / Pawn 重生后必须经过原版重绑，而非把不空的 GetSummoner 当作当前代次有效。

`CEC5E0` 的本轮直接 code xrefs 只有 `CF2F00:CF3005`、`CF3090:CF319D`。这说明已查标准路径的初始化属于一次召唤 activation；不声称已排除全游戏所有间接/蓝图旁路。

## 5. Start / Appear 为什么不是纯初始化

### 5.1 两条 Spirit 原生开始路径

`[E-IDA]` `CF2F00(Spirit*, SourceCommandAction*)` 与 `CF3090(Spirit*, SourceCommandAction*, CommandList*)` 均执行：

1. `CF6EB0` 解绑旧 source。若 Spirit 已处于 appeared 状态，先走 OnDisappear / 委托、清 CommandQueue、清 State、停移动、SetActive(false)。
2. 原生写入 source weak，查询两个召唤接口，调用 `CEC5E0` 建立 Summoner / 设置 / 委托。
3. 根据位置风格重置坐标；清 State，进入 appeared 状态，`CEFAE0(true)` 激活，调用 OnAppear 并广播 OnSpiritAppear。
4. 带列表的 `CF3090` 还清自己的队列、`CF8820` 从 Summoner 同步输入、`C977C0` 加入 command class 列表、`CC3970` 立即处理队列，然后 OnCommandAdded。

`[E-IDA]` command 侧 `D1D360` 从自身 Pawn（`+440`）取 Summoner，调用 `CE1920` 获取配置 SpiritClass，按 conditioned list 选择 `CF3090` 或 `CF2F00`；若配置要求则附着 Spirit 到该 Pawn。`D1D1A0` 的另一条召唤开始路径按输入槽取已装备 Spirit，然后进入 `CF3090`。这些函数位于 command 的原版执行链；本文不依靠 IDA 自动函数名给它们虚构公共 API。

`D1C3F0` 的初始化路径已经调用 `CE1920` 预备 Spirit，并预创建其命令对象，但没有运行上面的 Summoner bind / Appear。这进一步证明“有装备/有 Spirit”与“已进入具体召唤上下文”不能合并。

### 5.2 Appear / ResetLocation / SetActive 的边界

`[E-IDA]` `Appear` exec `DA1C70 → CCFCB0` 仅在非 appeared 时清 State、`CEE460 ResetLocation`、置 appeared、`CEFAE0(true)`、调用 Blueprint OnAppear 和委托。**它没有写 Summoner，也没有设 Faction。**

`ResetLocation CEE460` 必须先有有效 Summoner weak；它取 Summoner transform、按召唤位置设置/胶囊高度差求坐标，实际移动 Spirit，并进入原版移动/OnResetLocation 链。手动 teleport 到 Host row 位置不能替代此关系；反过来在 snapshot 写者仍运行时调 ResetLocation 又会增加位置写者。

`SetActive CEFAE0` 更新原生活跃状态、Actor hidden/tick/collision、按组件初始 tick 状态恢复组件，并处理控制器关联等。当前 `presentation_policy::apply` 关闭 Actor/Controller tick、非视觉组件 tick 和 RootMotionFactor；一次原版激活有能力重新改变其中的组件状态。`guard::mark` 不能被当作“任何原版初始化都绝对纯呈现”的证明。

`[E-CUE]` `BP_SpiritCharacterBase` 的 OnAppear 走完整 Ubergraph 入口 `1256`：调父事件、`1266 StopAllAnimations`、进入 renderer hidden/color 重置，再到 `649 SpawnEmitterAtLocation(AppearFX)`，停止 FadeOut、重设 Dissolve 并开始 FadeIn。OnCommandAdded 在 `1313/1350` ForceTick；OnDisappear / SoftDisappear 涉及停止动画、State、DisappearFX 与 FadeOut。因此直接 OnAppear 也不是无副作用的背景初始化：会覆盖现有 snapshot pose/visual，并制造一次性 FX。

## 6. 与当前 Client 的具体接线距离

`[E-源码]` 当前 `client.cpp::present_lilies` 对远端 seat 使用 `core::spawn::spawn(50+seat, position/skin)`。`core/spawn.cpp` 创建/保留 ELMP PC、创建 Pawn、调用真实 Possess，并核对 PC↔Pawn 双向关系。因此候选工厂有可用的对应 PC/Pawn 基础；不是必须再造一套 Controller。但这个路径没有恢复该远端的完整 profile / SpiritID / 装备等级 / summon set。

`client.cpp::present_spirits` 则按 `(seat,index)` 建表，`entities::spawn_copy(class,xyz)` 在 SpawnParameters.Owner 位置传 null，完成 Actor 生成后才应用 presentation policy、pose/visual/位置。它没有调用原版装备工厂、source command bind 或召唤 Start。这是本轮的确定性断链；不能由外观已经像魔女来推断上下文已经完成。

推荐的后续实施顺序 `[方案，尚未实施/实测]`：

1. 首先将目标限定为 **正确构造与归属**。在对应远端 PC/Pawn 完成 Possess 后，经已预检的 `CE1920` 获取 class Spirit，验证 actual class、owner/seat、Faction、weak serial 和一对一绑定；失败则不暴露半初始化对象。只把 Faction/所属工厂的恢复计为本阶段成果。
2. 显式拆出“原版装备组件拥有的 borrowed Spirit”生命周期，停止逐 row DestroyActor；先证明 map/PC teardown 的原版清理，跟踪 map/Pawn/PC/Actor 代次，失效重新获取。保持原有 Host pose/visual/Act 写者，构造后重新核对 presentation 禁止项，**不顺便调用 Appear 或 Start**。
3. 若需要真实 GetSummoner / input / summon settings / attachment，再定义 Host 可重建的 source CommandAction 和 activation 边界。源至少包括对应 Pawn、源 command class/原版初始化、装备/选择上下文及召唤阶段，不能从 Spirit class 单独反推。选择原版召唤执行为写者时，必须同时撤销同一技能的重复 Act / FX 生成路径；不是给当前冻结副本加一次 Start 就完成。
4. 未证明能安全拆开 activation 的入口，保持 fail closed。`CEC5E0` 虽短，也只是在完整 Start 事先设好 source/interfaces 后工作的内部 helper；不能靠手写这些内部字段来调用它，更不能修改 Client P1 的 source command 为远端建立上下文。

验收应分别记录同 class 的 Host/Client Spirit：wire seat/index、PC/Pawn/Spirit weak serial、Actor.Owner、Pawn.Controller、GetSummoner、Faction、原生 IsActive、source activation（若实施）、当前命令数、原版 Ability/FX 生成数。必须覆盖初次出现、连续复用、同行同 class、晚加入、Pawn 重生、换图/断开；Faction=1 通过不能替代 GetSummoner 或生命周期通过。本轮只保存静态证据，没有把这些候选验收写成已通过。
