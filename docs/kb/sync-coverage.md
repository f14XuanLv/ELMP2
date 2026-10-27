# 远程同步覆盖矩阵（P7 重启验收工具；ADR 0006）

- 每行一个同步点位。**状态**：`已同步`（剧本 + 对账 + 两侧截图证实）/ `部分`（有通道但有可见差异或只覆盖部分情形）/ `缺失`（Host 有、Client 无）/ `不同步`（机制 ⑤，理由进 README 已知限制）/ `待测`（尚无本阶段实测）。
- **机制**（ADR 0006）：①快照状态 ②事件复制 ③决策复制 ④放开部分执行层 ⑤不同步。
- **验证方法**：`rec` = 两侧逐帧变化沿记录（`elmp lan sync` 上线前用测试辅助 `%TEMP%\elmp_rec.lua` + `elmp_reccmp.py`）；`diff` = `tools/sync_diff.py`；`scn:<剧本>` = `tools/scenario.py <剧本>`；`shot` = 两侧成对截图；`cnt` = `elmp lan status` / `guard` 计数。
- **最近实测**写日期与证据位置；“基线”= P6（`4972eb0`）在 Host2 / Client2 上的状态。
- Host2 / Client2 存档（槽 0）里没有未开门 / 宝箱 / 可进入的未打 Boss（net-facts「G3 世界一致汇总」）：这些行若无用户存档，按同通路代证并在备注写「未在真对象上实测」。

## 远程莉莉（Host P1 副本、其他客人副本）

| 编号 | 点位 | Host 源头（证据） | 状态 | 机制 | Client 验证 | 最近实测 | 备注 |
|---|---|---|---|---|---|---|---|
| L01 | 位置 / 朝向 | 快照 `l` 行 `x y z facing`（`LocomotionComponent:IsFacingRight`）`[E-实测]` | 部分 | ① | diff（5 UU）、rec | 2026-10-28 基线：静止时 diff 一致；移动中 slime-fight / join 位置差 55–82 UU（副本直放 + 采样时刻差） | 副本每快照直放（无插值） |
| L02 | 速度 / 移动动画 idle · run | 轨道 0 名 `[E-实测]` | 部分 | ① | rec | 2026-10-28 基线：rec 中 idle / run 边一致（slime-fight l:1 93/122 按序匹配，差异为受击段） | 莉莉移动动画由输入驱动（net-facts「命令注入与输入驱动实验」），副本靠快照动画名 |
| L03 | jump_up / apex / down · landing | 轨道 0 名 | 部分 | ① | rec | 2026-10-28 基线：slime-fight l:1 缺 `jump_up`；r7-p6 一次一致 | `landing` 本次 Host 未出现 |
| L04 | dodge | 轨道 0 名 | 部分 | ① | rec | 2026-10-28 基线：r7-p6 一致；slime-fight 未单独计数 | 单次样本 |
| L05 | 攻击各段（`summon_DEFAULT_A/B/C`、`summon_air_*`） | 轨道 0 名；技能 `LaunchAbilityFromDescription` `0xCAD0C0` `[E-IDA]` | 部分 | ①② | rec、cnt（acts） | 2026-10-28 基线：slime-fight / r7-p6 `summon_DEFAULT_C`、`summon_air_*` 一致；段 A/B 未出现 | 段号 A/B 未覆盖（本剧本只出 C） |
| L06 | 其他召唤（SUMMON_1 / SUMMON_2 不同精灵）、HOOK | 同上 | 待测 | ①② | rec | — | |
| L07 | 祈祷（HEAL） | 轨道 0 名、`UseHeal` | 待测 | ① | rec | — | |
| L08 | 受伤（`damage_start / damage_loop`） | 轨道 0 名；HP 下降 | 部分 | ① + 受击表现 | rec、shot | 2026-10-28 基线：slime-fight Host P1 受击 21 次，Client 副本受击动画与 HP 序列一致（HP 序列晚开始 4 项为记录窗口差） | P7 实测缺（net-facts「P7 复盘」） |
| L09 | 击退 / 击飞（`damage_launch_*`） | 轨道 0 名；位置 | 部分 | ① | rec、diff | 2026-10-28 基线：`damage_launch_*` 出现在副本（slime-fight start sync） | |
| L10 | 死亡 | 轨道 0 名、`alive` | 部分 | ① | rec、scn:death-respawn | 2026-10-28 基线：death-respawn Host P1 死亡 l:1 34/35；客人自己的死亡见 G03 | |
| L11 | 复活 | 同上 | 部分 | ① | scn:death-respawn | 2026-10-28 基线：death-respawn 回椅子两侧一致（church_08 2/2） | |
| L12 | 坐椅休息（`rest_*`） | 轨道 0 名 | 待测 | ① | rec、shot | 2026-10-28 基线：world-objects `OnInteract` 打开休息菜单，两侧 P1 均站立，未出现 `rest_loop`；需原版坐下路径复测 | INTERACT 结算只在 Host |
| L13 | 交互动画（取物等） | 轨道 0 名 | 待测 | ① | rec | — | |
| L14 | 爬梯 / 抓墙 / 钩索 | 轨道 0 名；`OnWallgrabStarted / OnHookStarted` `[E-SDK]` | 待测 | ① | rec | — | church_07 无此地形，需选图 |
| L15 | 水中（若有） | `OnBreakWaterSurfaceDelegate` `[E-SDK]` | 待测 | ① | rec | — | 先确认测试存档可达的水域 |
| L16 | 精灵出现 / 消失 | 快照 `sp` 段（Owner = 座位 PC、未隐藏）`[E-实测]` | 已同步 | ① | rec、shot | 2026-10-28 基线：rec `s:1:BP_s5060_Assassin_C` 11/11、`s:2:…` 7/8；2026-10-28 位置插值：Host / Client 移动 72 帧、停顿 0（net-facts「精灵位置插值」） | P6 精灵副本 |
| L17 | 精灵动作 / 技能 | `sp` 行动画 + `Act`（Ref spirit） | 已同步 | ①② | rec、cnt | 2026-10-28 基线：同上（精灵动作序列按序一致）；2026-10-28 位置插值：Host / Client 移动 72 帧、停顿 0（net-facts「精灵位置插值」） | |
| L18 | 精灵位置插值 | `sp` 行位置 | 缺失 | ① | diff、trace | P6 已知：每快照直放 | |
| L19 | 伴随精灵（`BP_Companion_*`，常驻跟随的被动精灵）出现与粒子 | 待调研 | 已同步 | ① | rec（fx 按所属类） | 2026-10-28：伴随精灵编入精灵行（leader 座位），Host 可见 12 / Client 可见 12；截图 companions-*（net-facts「伴随精灵」） | 新增行（复盘发现） |
| L20 | 弹体 / 技能 Actor | `Act` 重放、`ActStop`（`AAbility::Stop` `0xCBDB80`） | 部分 | ② | cnt（acts / stops） | 2026-10-28 基线：guest-fight 结束 Client 6 个无 Owner 技能 Actor vs Host 1（重放体不设 Owner，2 s 寿命） | |
| L21 | 特效（`SpawnFX`） | `UBaseFXComponent::SpawnFX_Implementation` `0x7D0600` `[E-IDA]` | 部分 | ② | cnt、shot | 2026-10-28 基线：slime-fight Host fx_sent 137 / stops 133，Client fx_bad 0 | |
| L22 | 蓝图直生特效（`SpawnEmitter*`） | 待调研 | 缺失 | — | rec（fx） | P6 已知限制 | |
| L23 | 一次性音效 | FMOD `PlayEventAtLocation` `0x78D570` `[E-IDA]` | 部分 | ② | cnt | 2026-10-28 基线：slime-fight sounds_sent 90，Client sound_bad 0 | |
| L24 | 附着 / 循环音效 | `PlayEventAttached` `0x78D7A0` `[E-IDA]` | 已同步 | ② | cnt（attachedSounds） | 2026-10-28：Host 闪避 player-dodge 附着在 Lily → Client 副本播放；测试辅助史莱姆附着声音 → Client 史莱姆上 FMODAudioComponent 1；soundBad 0（net-facts「附着 / 循环音效」） | |
| L25 | 受击闪白 / 顿帧 | 受击处理 `BndEvt__HPComponent_…_HPDamageEvent`（`PlayAdditiveDamage`）`[E-IDA]`；顿帧 `AE_HitStop` | 部分 | ② | rec、shot | 2026-10-28 基线：Client hit_reacts 27（slime-fight） | |
| L26 | 皮肤 / 外观 | 快照 `skin` | 待测 | ① | shot | — | |
| L27 | HP / MP / 祈祷（HUD） | 快照 `hp mhp mp mmp heals mheals` | 部分 | ① | `elmp hud`、shot | 2026-10-28 基线：slime-fight 副本 HP 序列与 Host 一致 | P7 实测副本 HP 不更新 |
| L28 | 名字标签 | overlay | 待测 | ① | shot | — | |
| L29 | 切图 / 跟图淡入淡出 | `Map` / `MapReady` | 部分 | ① | scn:map-change、shot | 2026-10-28 基线：map-change 跟图 church_08→07 25/26 一致；截图 map-change-follow | |
| L30 | 动画相位（轨道时间） | 快照 `t` | 缺失 | ① | rec | P6 代码：副本换动画不 `SetTrackTime`（敌人有） | 新增行 |

## 敌人（出生点 / 动态生成 / Boss）

| 编号 | 点位 | Host 源头（证据） | 状态 | 机制 | Client 验证 | 最近实测 | 备注 |
|---|---|---|---|---|---|---|---|
| E01 | 存在 / 出现（出生点敌人） | registry Enemy + `SourceSpawnPoint` 名 `[E-实测]` | 已同步 | ① | diff（数量）、cnt（taken） | 2026-10-28 基线：六剧本 church_07 两侧 24 敌集合一致（missing 0 / extra 0，Host 击杀后 Client 隐藏保留） | |
| E02 | 动态生成敌人 | `ed` 段（enemy_dyn） | 待测 | ① | diff | — | |
| E03 | 死亡 / 消失 | `flags` bit2 dead、行消失 → 隐藏 | 部分 | ① | diff、rec | 2026-10-28 基线：Host 击杀 Slime21 → Client 隐藏（world-objects）；Host 死亡 Client 活 = 0 次 | Client 不跑原版死亡（v1 缺陷 1） |
| E04 | 位置 / 朝向 | `e` 行；Client 100 ms 缓冲插值、`SetFacing` | 部分 | ① | diff、trace | 2026-10-28 基线：静止一致；移动中差 36–200 UU = 100 ms 插值缓冲（预期） | |
| E05 | VisualPivot 竖直偏移 | 蓝图 ResetVisualPivotLocation（原生无引用 [E-IDA]） | 已同步 | 无需（敌人无非 0 写入者） | shot、diff | 2026-10-28：24 敌逐帧采样恒 0（史莱姆 300 帧 + 士兵 600 帧）；CUE 写入者只有重置与莉莉攀爬（net-facts「敌人 VisualPivot」） | |
| E06 | 动画全状态（idle / walk / idle_turn / attack_* / fall / landing …） | 轨道 0 名 + 时间 | 已同步 | ① | rec | 2026-10-28：史莱姆 98–100% / 士兵 18/18 按序一致，多余 <empty> 0（net-facts「敌人受击 / 动画全状态」） | 其他敌种待测 |
| E07 | 技能 / 弹体 | `Act`（`0xCAD0C0`）、`ActStop` | 部分 | ② | cnt | 2026-10-28 基线：acts 45 / Client act_bad 0 | |
| E08 | 特效 | `SpawnFX` | 部分 | ② | cnt | 2026-10-28 基线：同 L21 | |
| E09 | 一次性音效 | `PlayEventAtLocation` | 部分 | ② | cnt | 2026-10-28 基线：同 L23 | |
| E10 | 附着 / 循环音效 | `PlayEventAttached` | 已同步 | ② | cnt | 2026-10-28：Host 闪避 player-dodge 附着在 Lily → Client 副本播放；测试辅助史莱姆附着声音 → Client 史莱姆上 FMODAudioComponent 1；soundBad 0（net-facts「附着 / 循环音效」） | |
| E11 | 受击反应（附加受击动画 / 闪白） | Host HP 下降 → Client 调受击处理函数 | 已同步 | ② | rec、shot | 2026-10-28：史莱姆 98–100% / 士兵 18/18 按序一致，多余 <empty> 0（net-facts「敌人受击 / 动画全状态」） | |
| E12 | 硬直 / 击退 / 眩晕动画 | 轨道 0 名 | 待测 | ① | rec | — | |
| E13 | Boss 阶段 | 待调研 | 待测 | — | — | — | 需要用户存档 |
| E14 | Boss 血条 UI | 待调研 | 待测 | — | shot | — | 需要用户存档 |
| E15 | Boss 死亡慢动作 / 时间缩放 | `BP_Character_Boss_Base_C`（绑 Controller0）`[E-CUE]` | 待测 | — | shot | — | 需要用户存档 |
| E16 | 召唤物收养 / 销毁 | enemy_dyn 收养（≤300 UU 同类）/ 生成副本 | 待测 | ① | diff | — | |
| E17 | 敌人头顶 / 血条控件（若有） | `WidgetComponent` | 待测 | ① | shot | — | 先确认原版是否显示 |

## 世界对象

| 编号 | 点位 | Host 源头（证据） | 状态 | 机制 | Client 验证 | 最近实测 | 备注 |
|---|---|---|---|---|---|---|---|
| W01 | 门 | `ClearableComponent` IsCleared → Client `OnClearedStatusChecked(true)` `[E-实测]` | 待测 | ① | scn:world-objects、shot | — | 未在真对象上实测（无存档） |
| W02 | 宝箱 | 同上 | 待测 | ① | 同上 | — | 未在真对象上实测（无存档） |
| W03 | 可破坏物 | `w` 行 broken 位（Breakable.HPComponent ≤ 0）→ 隐藏 | 已同步 | ① | scn:world-objects | 2026-10-28 基线：world-objects 两朵花 Host 销毁 → Client 隐藏，未破坏件不变 | Client 不回放破坏（不生成掉落） |
| W04 | 拾取物 / 掉落 / 经验球 | `pk` 段 | 部分 | ① | scn:world-objects、rec（fx） | 2026-10-28 基线：world-objects 掉落 / 经验球粒子两侧都有；数量未逐个对账 | r7-p6 经验球粒子样本 Host 16 / Client 26 |
| W05 | 椅子点亮 | `BP_Interactable_RestingPoint_C` 状态 | 待测 | — | shot | — | |
| W06 | 椅子休息中 | 莉莉行动画（L12） | 待测 | ① | shot | 2026-10-28 基线：见 L12 | |
| W07 | 机关 / 开关 / 升降台 | 待调研 | 待测 | — | shot | — | |
| W08 | 剧情旗标 / 教程 / 对话触发结果 | `w` 行 story `value`（条件结果）/ Trigger `OnClearedStatusChecked` | 待测 | ① | scn:world-objects | — | |
| W09 | 区域名显示 | `0xD0B8A0` 按区域查表（前台语义）`[E-IDA]` | 待测 | — | shot | — | |
| W10 | 地图解锁 | 待调研 | 待测 | — | — | — | |
| W11 | 存档点（Host 保存结果） | Host `SaveGame`；Client guard 拒绝 | 待测 | ⑤（Client 不存档） | cnt（saveBlocked）、哈希 | — | |
| W12 | 假墙 / 世界触发器 | worldobj trigger → `OnClearedStatusChecked` `[E-实测]` | 待测 | ① | shot | — | |
| W13 | 交互物消耗（Tip 等） | worldobj gone 位 → 隐藏 | 待测 | ① | shot | — | |

## 客人自身

| 编号 | 点位 | Host 源头（证据） | 状态 | 机制 | Client 验证 | 最近实测 | 备注 |
|---|---|---|---|---|---|---|---|
| G01 | HP / MP / 祈祷 HUD | 自己座位 `l` 行 → 本地 P1 属性直写 | 部分 | ① | `elmp hud`、shot | 2026-10-28 基线：Client HUD HP 取 Host 座位值（guest-fight） | |
| G02 | 受伤表现（本地 P1 受击动画 / 击退） | Host 座位受击 | 待测 | — | rec、shot | — | 本地 P1 是预测体，Host 结算 |
| G03 | 死亡 / 复活流程 | Host 座位死亡 → party/respawn | 缺失 | ① | scn:guest-fight | 2026-10-28 基线：guest-fight / death-respawn 两次：客人本地 P1 无 `death` 动画（Host 座位 `death`），复活后一致 | |
| G04 | 升级 / 装备反馈 | `EquipRequest / EquipResult` | 待测 | ① | cnt | — | |
| G05 | 相机 | 本地镜头只跟本地 P1 | 待测 | — | shot | — | |
| G06 | Host 暂停 / 菜单时 Client 看到什么 | 待调研 | 待测 | — | shot | — | |
| G07 | Client 暂停时 Host 不受影响 | 待调研 | 待测 | — | cnt | — | |
| G08 | 本地 P1 预测与回拉 | >150 UU 回拉 | 部分 | ① | diff、cnt（reconciles） | 2026-10-28 基线：reconciles 计数 3–4；guest-fight l:2 缺 `jump_up_air / wall_climb_high`（预测体） | |

## 全局

| 编号 | 点位 | Host 源头（证据） | 状态 | 机制 | Client 验证 | 最近实测 | 备注 |
|---|---|---|---|---|---|---|---|
| X01 | 顿帧 / 时间缩放 | `AE_HitStop`（表现类 AE）、待调研 | 待测 | — | rec | — | |
| X02 | Boss 慢动作 | 见 E15 | 待测 | — | — | — | 需要用户存档 |
| X03 | BGM 区域切换 | `0xD0B8A0` 区域表 `[E-IDA]` | 待测 | — | 听 / 日志 | — | |
| X04 | 环境音区域切换 | 待调研 | 待测 | — | — | — | |
| X05 | 后台图（客人所在、P1 不在）各行降级 | P3 后台图只加载 GEO / DESIGN / GAMEPLAY | 部分 | ⑤ / ① | scn:map-change | 2026-10-28 基线：map-change P2 独自后台 church_07：按槽过滤后 25/25 一致；Host P1 进入后移交 25/26 | 无音效 / 剧情事件 / Boss（已知限制） |

## 正确性行（每次实测都要带）

| 编号 | 点位 | 判据 | 状态 | Client 验证 | 最近实测 | 备注 |
|---|---|---|---|---|---|---|
| C01 | Client 结算泄漏 | guard `damage / skill / save / death` 放行 = 0（`replayed` 计数为 0） | 已同步 | cnt | 2026-10-28 基线：六剧本 Client guard `damage_replayed=0 skill_replayed=0 save_blocked=0`（damage_blocked 最多 42） | |
| C02 | 敌人数量 / HP 全等 | 两侧同图敌人集合一致、HP 全等（静止或采样同刻） | 已同步 | diff | 2026-10-28 基线：六剧本敌人集合与 HP 全等（hp 差 0） | |
| C03 | Client 世界清除 / 存档不变 | Client `Saved/SaveGames` 与 `Saved/ELMP` 哈希不变；出生点清除数不增 | 已同步 | 哈希 | 2026-10-28 基线：六剧本 Client `SaveGames` / `Saved/ELMP/ELMP` 哈希不变（scenario.py C03） | |
| C04 | 死亡 / 复活后各行 | 重跑 C01–C03 + L10 / L11 | 部分 | scn:death-respawn | 2026-10-28 基线：death-respawn 正确性行绿；G03 缺失 | |
| C05 | 切图后各行 | 同上 + L29 | 已同步 | scn:map-change | 2026-10-28 基线：map-change 正确性行绿 | |
| C06 | 晚加入 | 同上 | 已同步 | scn:join-reconnect | 2026-10-28 基线：join-reconnect 晚加入 24/26（运动滞后）正确性绿 | |
| C07 | 重连 | 同上 | 部分 | scn:join-reconnect | 2026-10-28 基线：断线后 Host parked=1、重连收回；本次重连时 Host P1 已在无人操作中战死回 church_08（2/2 一致），church_07 重连待复测 | |
| C08 | 两客人（Client3） | 高可见度行 + C01–C03 | 待测 | scn:* ×3 实例 | — | |
