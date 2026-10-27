# 远程同步覆盖矩阵（P7 重启验收工具；ADR 0006）

- 每行一个同步点位。**状态**：`已同步`（剧本 + 对账 + 两侧截图证实）/ `部分`（有通道但有可见差异或只覆盖部分情形）/ `缺失`（Host 有、Client 无）/ `不同步`（机制 ⑤，理由进 README 已知限制）/ `待测`（尚无本阶段实测）。
- **机制**（ADR 0006）：①快照状态 ②事件复制 ③决策复制 ④放开部分执行层 ⑤不同步。
- **验证方法**：`rec` = 两侧逐帧变化沿记录（`elmp lan sync` 上线前用测试辅助 `%TEMP%\elmp_rec.lua` + `elmp_reccmp.py`）；`diff` = `tools/sync_diff.py`；`scn:<剧本>` = `tools/scenario.py <剧本>`；`shot` = 两侧成对截图；`cnt` = `elmp lan status` / `guard` 计数。
- **最近实测**写日期与证据位置；“基线”= P6（`4972eb0`）在 Host2 / Client2 上的状态。
- Host2 / Client2 存档（槽 0）里没有未开门 / 宝箱 / 可进入的未打 Boss（net-facts「G3 世界一致汇总」）：这些行若无用户存档，按同通路代证并在备注写「未在真对象上实测」。

## 远程莉莉（Host P1 副本、其他客人副本）

| 编号 | 点位 | Host 源头（证据） | 状态 | 机制 | Client 验证 | 最近实测 | 备注 |
|---|---|---|---|---|---|---|---|
| L01 | 位置 / 朝向 | 快照 `l` 行 `x y z facing`（`LocomotionComponent:IsFacingRight`）`[E-实测]` | 待测 | ① | diff（5 UU）、rec | — | 副本每快照直放（无插值） |
| L02 | 速度 / 移动动画 idle · run | 轨道 0 名 `[E-实测]` | 待测 | ① | rec | — | 莉莉移动动画由输入驱动（net-facts「命令注入与输入驱动实验」），副本靠快照动画名 |
| L03 | jump_up / apex / down · landing | 轨道 0 名 | 部分 | ① | rec | 2026-10-28 r7-p6：`jump_up jump_apex jump_down` 一致 | `landing` 本次 Host 未出现 |
| L04 | dodge | 轨道 0 名 | 部分 | ① | rec | 2026-10-28 r7-p6：一致 | 单次样本 |
| L05 | 攻击各段（`summon_DEFAULT_A/B/C`、`summon_air_*`） | 轨道 0 名；技能 `LaunchAbilityFromDescription` `0xCAD0C0` `[E-IDA]` | 部分 | ①② | rec、cnt（acts） | 2026-10-28 r7-p6：10 次 `summon_DEFAULT_C` + `summon_air_DEFAULT_A` 一致 | 段号 A/B 未覆盖（本剧本只出 C） |
| L06 | 其他召唤（SUMMON_1 / SUMMON_2 不同精灵）、HOOK | 同上 | 待测 | ①② | rec | — | |
| L07 | 祈祷（HEAL） | 轨道 0 名、`UseHeal` | 待测 | ① | rec | — | |
| L08 | 受伤（`damage_start / damage_loop`） | 轨道 0 名；HP 下降 | 待测 | ① + 受击表现 | rec、shot | — | P7 实测缺（net-facts「P7 复盘」） |
| L09 | 击退 / 击飞（`damage_launch_*`） | 轨道 0 名；位置 | 待测 | ① | rec、diff | — | |
| L10 | 死亡 | 轨道 0 名、`alive` | 待测 | ① | rec、scn:death-respawn | — | |
| L11 | 复活 | 同上 | 待测 | ① | scn:death-respawn | — | |
| L12 | 坐椅休息（`rest_*`） | 轨道 0 名 | 待测 | ① | rec、shot | — | INTERACT 结算只在 Host |
| L13 | 交互动画（取物等） | 轨道 0 名 | 待测 | ① | rec | — | |
| L14 | 爬梯 / 抓墙 / 钩索 | 轨道 0 名；`OnWallgrabStarted / OnHookStarted` `[E-SDK]` | 待测 | ① | rec | — | church_07 无此地形，需选图 |
| L15 | 水中（若有） | `OnBreakWaterSurfaceDelegate` `[E-SDK]` | 待测 | ① | rec | — | 先确认测试存档可达的水域 |
| L16 | 精灵出现 / 消失 | 快照 `sp` 段（Owner = 座位 PC、未隐藏）`[E-实测]` | 待测 | ① | rec、shot | — | P6 精灵副本 |
| L17 | 精灵动作 / 技能 | `sp` 行动画 + `Act`（Ref spirit） | 待测 | ①② | rec、cnt | — | |
| L18 | 精灵位置插值 | `sp` 行位置 | 缺失 | ① | diff、trace | P6 已知：每快照直放 | |
| L19 | 伴随精灵（`BP_Companion_*`，常驻跟随的被动精灵）出现与粒子 | 待调研 | 缺失 | — | rec（fx 按所属类） | 2026-10-28 r7-p6：Client 0 / Host 150 粒子样本 | 新增行（复盘发现） |
| L20 | 弹体 / 技能 Actor | `Act` 重放、`ActStop`（`AAbility::Stop` `0xCBDB80`） | 部分 | ② | cnt（acts / stops） | 2026-10-28 r7-p6：技能 63 / 重放 64、stop 61 | |
| L21 | 特效（`SpawnFX`） | `UBaseFXComponent::SpawnFX_Implementation` `0x7D0600` `[E-IDA]` | 部分 | ② | cnt、shot | 2026-10-28 r7-p6：fx 126 / 重放 127 | |
| L22 | 蓝图直生特效（`SpawnEmitter*`） | 待调研 | 缺失 | — | rec（fx） | P6 已知限制 | |
| L23 | 一次性音效 | FMOD `PlayEventAtLocation` `0x78D570` `[E-IDA]` | 部分 | ② | cnt | 2026-10-28 r7-p6：58 / 54（`soundFar=4` 超距不播） | |
| L24 | 附着 / 循环音效 | `PlayEventAttached` `0x78D7A0` `[E-IDA]` | 缺失 | — | cnt（attachedSounds） | 2026-10-28 r7-p6：Client `attachedSounds=1` 未复制 | |
| L25 | 受击闪白 / 顿帧 | 受击处理 `BndEvt__HPComponent_…_HPDamageEvent`（`PlayAdditiveDamage`）`[E-IDA]`；顿帧 `AE_HitStop` | 待测 | ② | rec、shot | — | |
| L26 | 皮肤 / 外观 | 快照 `skin` | 待测 | ① | shot | — | |
| L27 | HP / MP / 祈祷（HUD） | 快照 `hp mhp mp mmp heals mheals` | 待测 | ① | `elmp hud`、shot | — | P7 实测副本 HP 不更新 |
| L28 | 名字标签 | overlay | 待测 | ① | shot | — | |
| L29 | 切图 / 跟图淡入淡出 | `Map` / `MapReady` | 待测 | ① | scn:map-change、shot | — | |
| L30 | 动画相位（轨道时间） | 快照 `t` | 缺失 | ① | rec | P6 代码：副本换动画不 `SetTrackTime`（敌人有） | 新增行 |

## 敌人（出生点 / 动态生成 / Boss）

| 编号 | 点位 | Host 源头（证据） | 状态 | 机制 | Client 验证 | 最近实测 | 备注 |
|---|---|---|---|---|---|---|---|
| E01 | 存在 / 出现（出生点敌人） | registry Enemy + `SourceSpawnPoint` 名 `[E-实测]` | 待测 | ① | diff（数量）、cnt（taken） | — | |
| E02 | 动态生成敌人 | `ed` 段（enemy_dyn） | 待测 | ① | diff | — | |
| E03 | 死亡 / 消失 | `flags` bit2 dead、行消失 → 隐藏 | 待测 | ① | diff、rec | — | Client 不跑原版死亡（v1 缺陷 1） |
| E04 | 位置 / 朝向 | `e` 行；Client 100 ms 缓冲插值、`SetFacing` | 待测 | ① | diff、trace | — | |
| E05 | VisualPivot 竖直偏移 | 待调研 | 缺失 | — | shot、diff | P6 已知限制 | |
| E06 | 动画全状态（idle / walk / idle_turn / attack_* / fall / landing …） | 轨道 0 名 + 时间 | 部分 | ① | rec | 2026-10-28 r7-p6：战斗中 5 只史莱姆序列逐项一致（含 `idle_turn`） | 其他敌种待测 |
| E07 | 技能 / 弹体 | `Act`（`0xCAD0C0`）、`ActStop` | 部分 | ② | cnt | 2026-10-28 r7-p6 | |
| E08 | 特效 | `SpawnFX` | 部分 | ② | cnt | 2026-10-28 r7-p6 | |
| E09 | 一次性音效 | `PlayEventAtLocation` | 部分 | ② | cnt | 2026-10-28 r7-p6 | |
| E10 | 附着 / 循环音效 | `PlayEventAttached` | 缺失 | — | cnt | P6 已知限制 | |
| E11 | 受击反应（附加受击动画 / 闪白） | Host HP 下降 → Client 调受击处理函数 | 待测 | ② | rec、shot | — | |
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
| W03 | 可破坏物 | `w` 行 broken 位（Breakable.HPComponent ≤ 0）→ 隐藏 | 待测 | ① | scn:world-objects | — | Client 不回放破坏（不生成掉落） |
| W04 | 拾取物 / 掉落 / 经验球 | `pk` 段 | 待测 | ① | scn:world-objects、rec（fx） | — | r7-p6 经验球粒子样本 Host 16 / Client 26 |
| W05 | 椅子点亮 | `BP_Interactable_RestingPoint_C` 状态 | 待测 | — | shot | — | |
| W06 | 椅子休息中 | 莉莉行动画（L12） | 待测 | ① | shot | — | |
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
| G01 | HP / MP / 祈祷 HUD | 自己座位 `l` 行 → 本地 P1 属性直写 | 待测 | ① | `elmp hud`、shot | — | |
| G02 | 受伤表现（本地 P1 受击动画 / 击退） | Host 座位受击 | 待测 | — | rec、shot | — | 本地 P1 是预测体，Host 结算 |
| G03 | 死亡 / 复活流程 | Host 座位死亡 → party/respawn | 待测 | ① | scn:guest-fight | — | |
| G04 | 升级 / 装备反馈 | `EquipRequest / EquipResult` | 待测 | ① | cnt | — | |
| G05 | 相机 | 本地镜头只跟本地 P1 | 待测 | — | shot | — | |
| G06 | Host 暂停 / 菜单时 Client 看到什么 | 待调研 | 待测 | — | shot | — | |
| G07 | Client 暂停时 Host 不受影响 | 待调研 | 待测 | — | cnt | — | |
| G08 | 本地 P1 预测与回拉 | >150 UU 回拉 | 待测 | ① | diff、cnt（reconciles） | — | |

## 全局

| 编号 | 点位 | Host 源头（证据） | 状态 | 机制 | Client 验证 | 最近实测 | 备注 |
|---|---|---|---|---|---|---|---|
| X01 | 顿帧 / 时间缩放 | `AE_HitStop`（表现类 AE）、待调研 | 待测 | — | rec | — | |
| X02 | Boss 慢动作 | 见 E15 | 待测 | — | — | — | 需要用户存档 |
| X03 | BGM 区域切换 | `0xD0B8A0` 区域表 `[E-IDA]` | 待测 | — | 听 / 日志 | — | |
| X04 | 环境音区域切换 | 待调研 | 待测 | — | — | — | |
| X05 | 后台图（客人所在、P1 不在）各行降级 | P3 后台图只加载 GEO / DESIGN / GAMEPLAY | 待测 | ⑤ / ① | scn:map-change | — | 无音效 / 剧情事件 / Boss（已知限制） |

## 正确性行（每次实测都要带）

| 编号 | 点位 | 判据 | 状态 | Client 验证 | 最近实测 | 备注 |
|---|---|---|---|---|---|---|
| C01 | Client 结算泄漏 | guard `damage / skill / save / death` 放行 = 0（`replayed` 计数为 0） | 待测 | cnt | — | |
| C02 | 敌人数量 / HP 全等 | 两侧同图敌人集合一致、HP 全等（静止或采样同刻） | 待测 | diff | — | |
| C03 | Client 世界清除 / 存档不变 | Client `Saved/SaveGames` 与 `Saved/ELMP` 哈希不变；出生点清除数不增 | 待测 | 哈希 | — | |
| C04 | 死亡 / 复活后各行 | 重跑 C01–C03 + L10 / L11 | 待测 | scn:death-respawn | — | |
| C05 | 切图后各行 | 同上 + L29 | 待测 | scn:map-change | — | |
| C06 | 晚加入 | 同上 | 待测 | scn:join-reconnect | — | |
| C07 | 重连 | 同上 | 待测 | scn:join-reconnect | — | |
| C08 | 两客人（Client3） | 高可见度行 + C01–C03 | 待测 | scn:* ×3 实例 | — | |
