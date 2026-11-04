> 2026-11-04 更正：本文保留历史观察；统一输入充分性、现有 guard 覆盖、renderer 偏移来源等推断已重新核验。当前事实以 [玩家图谱](player-sync-graph-v2.md)、[敌人图谱](enemy-sync-graph-v2.md) 和 [ADR 0008](../adr/0008-authoritative-presentation.md) 为准。

# Host 的莉莉在客户端为什么变黑、还高半身（2026-10-30，P8 莉莉侧）

> 这条是 `enemy-fx-graph.md` 的姐妹篇：那边是敌人（红光 = 材质参数），这边是**玩家的呈现副本**。
> 两个症状同时出现——"全身黑色只剩轮廓" + "凭空高一个身位"——看起来像一件事，实际是**两个独立的蓝图默认值丢了**。

## 1. 结论

客户端上呈现 Host 莉莉的是一个自建 Pawn（`BP_p0000_Lily_C` 的副本，slot = 50 + 座位号）。
它丢了两样**不是运行时状态、而是蓝图组件模板值**的东西：

| 症状 | 真实差异（实测） | 蓝图模板值 |
|---|---|---|
| 全黑剪影 | `ColorComponent` 当前颜色 **(0,0,0,1)**，原版 **(1,1,1,1)** | 材质按颜色相乘 → 黑 |
| 高一个身位 | `SpineSkeletonRenderer.RelativeLocation.Z` = **0**，原版 **-65** | `BP_p0000_Lily_C:SpineSkeletonRenderer_GEN_VARIABLE.RelativeLocation = (0,0,-65)`（继承自 `BP_Character_Player_Base`） |

比的是同一个客户端上的**两只**莉莉（本地玩家 = 原版，副本 = 我们 spawn 的），所以不涉及 Host/Client 差异：

```lua
-- 客户端 Dev eval：逐只读数值
for _, p in ipairs(FindAllOf('BP_p0000_Lily_C')) do
  local r = p.SpineSkeletonRenderer
  print(p:K2_GetActorLocation().Y, r.RelativeLocation.Z, p.ColorComponent:GetCurrentColor().R)
end
-- 结果：本地玩家 y=5474 relZ=-65 col=1.00 ；副本 y=5759 relZ=0 col=0.00
```

## 2. 被证伪的三条假设（先排掉，才不白改）

| 假设 | 怎么证伪 |
|---|---|
| 贴图 / atlas 没加载（黑 = 材质无贴图） | 把原版棋子的 `GetMaterial(0)` 直接 `SetMaterial` 到副本上，截图**毫无变化**；两边的 atlas 资产名同为 `p0001_Lily-atlas`（`elmp lily` 读回的 `skin=1`） |
| `ReplaceSpineData` 之后缺 `RequestRefreshPlayingAnimation`（原版 `SetSpineData` 里有这一句） | 在副本上手动调它，`RelativeLocation.Z` 仍是 0（动画确实在播，位置没变） |
| 是 Spine 动画/骨骼把整只抬起来了 | 副本与 Host 的 `row.anim`、track time、角色世界坐标（`y=5759, z=-332.8`）**完全一致**；差异只在该组件自己的相对变换 |

## 3. 修复：从同类 CDO 把模板值抄回来

`core::skin::normalize(pawn)`（`native/core/skin.cpp`）：

1. 用 `actor_component_by_class` 找到副本的 `SpineSkeletonRendererComponent`；
2. 用同类 CDO（`<pkg>.Default__<Class>`）上的同名组件读 `RelativeLocation`（schema 的 `scene_rel_location`），
   与副本不同就写回，并调 `K2_SetRelativeLocation` 让组件重算变换；
3. `ColorComponent->ClearColor(0)`（语义 = 回默认色，实测读回 `(1,1,1,1)`），只在当前颜色接近黑时做。

调用点：

- `coop::client::present_lilies` 创建副本时；
- `coop::entities::spawn_copy`（敌人 / 精灵副本）出生后；
- 副本创建后 **240 帧内每 20 帧重试一次**：只在"过图/读档窗口"里丢值，早期可能组件还没建好，
  重试把这段窗口覆盖掉（日志会打 `coop: renderer offset of … restored to (0,0,-65)`）。

不写死 `-65`：值来自 CDO，换皮肤/换骨架也对。

## 4. 验证

- 部署后新建的副本：出生即 `relZ=-65, col=1.00`，肉眼与本地玩家一致（截图 `l7-after-z-client.png` 对照）。
- 日志无 "restored" 行 = 这次出生时值本来就是对的（重试是保险）。

## 7. 后记（2026-10-30 深夜）：真正的"高一个身位"是踩地偏移 + 一个掩码溢出

`normalize` 修好"黑"之后，副本仍然高一个身位。继续量，拿到决定性数据（同一时刻、同一张图）：

| | Host 的莉莉 | Client 的副本 |
|---|---|---|
| 角色世界坐标 z | 467.2 | 467.2（一致） |
| `SpineSkeletonRenderer.RelativeLocation.Z` | **-65** | **0** |
| 渲染器世界 z | 402.2 | 467.2 ← 高 65 |

-65 / -50（敌人）这类"踩地偏移"是 **`USpineAnimationComponent::OnAfterBoneTransformUpdate`** 在骨骼更新后
按骨架把渲染器往下挪出来的（莉莉 -65、`e2021_Soldier` -50，每个骨架不同）。客户端为了不让状态机抢轨道
把这条 tick 关了，于是这段永远不发生。

修法（复制因，不写死数值）：Host 每帧采样 `SpineSkeletonRenderer.RelativeLocation.Z`，作为 `zoff`（0.1 UU）
随 `LilyRow` / `EnemyRow` 下发；客户端用 `core::skin::set_renderer_z_comp` 写到渲染组件上。

**这一版第一次发不出去**，原因值得记一笔：`net/snapshot.cpp` 的 delta 掩码是 `std::uint16_t`，
`LilyRow` 原本正好 16 个字段，加了第 17 个（`zoff`）以后 `bit <<= 1` 溢出，第 17 位永远是 0 ——
字段**静默**不参与 diff、也不写进包，客户端收到 0。敌方行只有 13 个字段，所以敌人一次就修好了，
莉莉没有。修法：掩码改 32 位 + varint，并给每张快照表加 `static_assert(tuple_size <= 32)`。

验证（部署后，两端逐字段对比）：

```
HOST  : y=-1957.1 actorZ=467.2 relZ=-65.0 worldZ=402.2
CLIENT: y=-1957.1 actorZ=467.2 relZ=-65.0 worldZ=402.2   ← 完全一致
```

## 8. 回归分析：4972eb0..HEAD 里到底是哪一刀把权威弄丢的

用 `git log -S` 定位到唯一一处：**`e230434`（P8 模拟模式）改了 `freeze()` 这个被两种对象共用的辅助函数**。

P6（4972eb0）的 `freeze()`：只放行呈现白名单（`SpineSkeletonAnimationEx` / `SpineSkeletonRenderer` /
`ParticleSystem` / `Widget` / `Bone*`），其余一律关，并且 `SetActorTickEnabled(false)` **无条件**——
任何副本的 Pawn Tick 都关，位置只能由快照写。

`e230434` 之后：

```cpp
if ((g_sim_local && cls != L"SpineAnimationComponent") || ...) 开   // 除 SpineAnimation 外全开
f.arg_bool(0, !g_sim_local);                                       // simLocal 时 Pawn Tick 也开
```

`freeze()` 被 `take()`（敌人）和 `present_lilies()`（玩家副本）**共用**。于是打开 `simLocal` 做敌人域实验时，
Host 莉莉的副本也被一起解冻：`ZenithCharacterMovementComponent` 开始逐帧跑重力/落地，与每帧的快照传送互相
拉扯——站着看不出来，跳跃/下落时副本被本地重力多拉一段，实测偏低 **69–85 UU**（约一个身位）。这就是
"有时候偏低一个身位"。

而"一直偏高一个身位"是另一个更老、与这次无关的问题：渲染器"踩地"偏移（`USpineAnimationComponent` 在骨骼
更新后把 `SpineSkeletonRenderer` 下移 -65）因为副本被冻结而从不发生——P6 起就存在，只是没人量过；我第一次
修它时又被 16 位掩码吃掉（见上一节）。

**修法（结构，不是补丁）**：`freeze()` 拆成两种模式，语义写进类型：

| 模式 | 谁用 | 放行什么 | Pawn Tick | 位移 |
|---|---|---|---|---|
| `Mode::Presentation` | 其它玩家的莉莉副本、接管的精灵/世界物 | 只有 Spine 单件 + 粒子 + HUD 组件 | 关 | 只由快照写 |
| `Mode::Simulated` | 敌人域（`simLocal`） | 上面的 + 命令队列/状态/技能 | 开（`UpdateCommands` 要它） | **仍然关**：变换归 Host |

关键一条：**位移组件在任何模式下都不开**。`simLocal` 只允许"命令/表现"在本地跑，**变换始终是 Host 权威**。

验证（部署后两端同一时刻逐字段）：

```
HOST  : y=-1957.1 z=467.2 relZ=-65.0 tick=true
CLIENT: y=-1957.1 z=467.2 relZ=-65.0 tick=false   ← 副本 tick 已关，三项完全一致
```

## 9. 还没解释的

**为什么模板值会丢**：`elmp spawn 101 baseline`（走同一个 `core::spawn` 路径）出生就是 `-65`，
只有"会话中、过图窗口里"新建的副本会变成 `0`。目前用"出生后重抄 + 数秒重试"绕过去；
真正的机制要等一次带日志的过图复现（怀疑与过图时 CDO/组件模板的可用时机有关），先记录在案。

## 10. 同一类问题的清单（后续按这个查）

| 表现载体 | 归属 | 现状 |
|---|---|---|
| `BattleColorComponent`（敌人红光材质参数） | 材质状态 | 已按 bit3 复制因（见 `enemy-fx-graph.md`） |
| `ColorComponent`（受击染色 / 默认色） | 材质状态 | 默认色已在副本上修正；受击闪白走 `hit_react` 复现 |
| `SpineSkeletonRenderer.RelativeLocation` 等组件模板值 | 蓝图模板 | 已用 `skin::normalize` 抄回 |
| 墙抓时的"狼" | 待定：Lily 同一骨架的动画（track 0）还是会另生成 Actor | 待复现取证：Host 抓墙时读 `GetCurrent(0)` 的动画名，并截图对比 |
| 客户端看敌人"普遍高一个身位" | 待定 | 待用同样方法量：客户端敌人的 `SpineSkeletonRenderer.RelativeLocation`、`row.z` 与 Host 的差 |

> 方法：这类"看起来是动画/特效"的问题，先量**同一进程里两只同类 Actor 的数值差**（本地原版 vs 自建副本），
> 就能把"同步没到"和"本地初始化不对"分开——这次两个症状都属于后者。
