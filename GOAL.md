# GOAL：P8 复制因不复制果 —— 逐域把表现交还给客户端模拟（2026-10-29）

> 本文件是 `/goal` 的完整计划。每轮开始前先读本文件与 §1；计划有变先改本文件（标日期）。
> **方法论以 `docs/adr/0007-replicate-causes.md` 为准**，本文件只记阶段目标、步骤与证据。
> **进度以 §5 勾选表为准**：完成一项打勾并补「日期 · 证据 · commit」。

**当前进行中**：V2 —— 客户端敌人动画由状态机驱动（代码已接，干净状态下的证据待补）
**需要用户**：① 带梯子 / 攀爬 / 祈祷点的存档；② 入口图里有未开门 / 宝箱 / 可进入未打 Boss 的存档。只影响对应域，不阻塞 V2。

## 0. 三条线（用户实机，2026-10-29）

| 线 | 分支 | 机制 | 实机 |
|---|---|---|---|
| P6 | `main` 基线 `4972eb0` | 冻结模拟 + 复制产物 | 完整但手感差 |
| P7-1 | `explore/p7-failed-client-vanilla-execution` | 跑执行层，但复制单位停在命令层、删通道太早 | 手感最好，完整性差 |
| P7-2 | `explore/p7-failed-snapshot-coverage` | P6 + 覆盖矩阵逐点补 | 两头都输 |

## 1. 方法论（摘要；全文见 ADR 0007）

- **复制因，不复制果**：Host 只发决策与状态；表现（Spine / 通知粒子 / 技能特效 / 音效）由客户端跑同一条原版管线生成。
- **因分两层**：敌人的命令启动沿（决策在 Host AI）、莉莉的输入快照（决策在输入）、以及命令累积出来的**状态集合**。
- **表现通道归零**：不新增通道；Fx / Snd 只作临时兜底，域内删得掉才算接上。
- **结算收窄**：效果施加 / 死亡 / 存档 / 交互副作用 / AI 激活，逐点拦、带计数、未知 fail closed。
- **验收 = 删除测试**：关掉该域复制通道后表现还在且与 Host 一致；不在，缺口在模拟层。

## 2. 已验证的事实（`docs/kb/pipeline-facts.md`）

- 管线：`ZenithCharacter::Tick` → `TryAddCommands` → `UpdateCommands` → `CommandAction` 五时间线（State / Ability / Move / FX / Sound）→ `UStateComponent` → `SpineAnimationComponent` → 动画通知 → 粒子。
- FX 的真实汇点是 `UGameplayStatics::SpawnEmitterAttached/AtLocation`；P6 只钩了包装层 `SpawnFX`，所以永远漏。
- 判决实验：`fxLocal=1`（关通道 + 关抑制）下，P6 的客户端敌人自己不产生特效（`localKept=0`）；把同一条命令注入客户端敌人则 `localKept=1`。
- 端到端：Host 命令启动沿 → `Msg::Cmd` → Client `AddNewImmediateCommand`（`cmds replayed=1`），`fxLocal=1` 下 FX 通道 0、本地生成 1。
- 状态漂移实测：把状态机打开后，之前实验遗留的敌人一直停在 `attack_A_fall_loop`，不自愈 → 必须有状态集合校正。

## 3. 步骤

**V1 敌人域：命令驱动（已通，待补删除测试的完整证据）**
- [x] `ProcessCommand` 启动沿捕获 + `Msg::Cmd` + 客户端 `AddNewImmediateCommand`（`30cac60`）
- [x] `fxLocal` 开关；`fxLocal=1` 下 Host 1/1 → Client replayed 1、本地 FX 1、通道 0
- [ ] 干净状态下重跑一次（新进图），两侧截图 + 变化沿对账

**V2 敌人域：动画交给状态机**
- [x] `animLocal` 开关：客户端不写快照轨道名；`freeze` 放行 `SpineAnimationComponent`
- [ ] 干净状态下验证：idle → 攻击动画 → idle，且关掉快照动画名后仍正确
- [ ] 失败则找模拟层缺口（`UStateComponent` 是否被喂、动画组件 tick、通知开关），记 kb

**V3 状态集合校正**
- [ ] 快照带敌人 `UStateComponent.ActiveStates` 的类名集合（Ref + 集合）
- [ ] 客户端在“无进行中命令且集合不一致”时校正，观察是否收敛 / 抖动
- [ ] 判据：60 s 战斗后集合一致；动画不卡在动作状态

**V4 交互减法（休息点 / 梯子 / 祈祷）**
- [ ] CUE 读 `BP_Interactable_RestingPoint` 的 ubergraph，切出“状态 + 动画”与“存档 / 清除 / 治疗”
- [ ] 客户端只跑前者；判据：两侧状态一致、Client 存档哈希不变

**V5 远程莉莉（输入复制）与精灵**
- [ ] 莉莉副本改喂输入快照 + 抑制本地命令生成（复用 P7-1 的知识，不照搬）
- [ ] 精灵由副本自己的召唤命令派生

**V6 通道归零**
- [ ] 每域通过删除测试后删掉对应 Fx / Snd 分支；矩阵从验收口径降级为回归检查表

## 4. 验收与铁律

- 每步都带：删除测试结果、两侧帧时间（P99 < 1 ms）、Client 结算泄漏 0、`Saved/` 哈希不变。
- 不碰正式游戏目录 / 存档；测前备份实例 `Saved/`；用户在用电脑时不做前台自动化。
- 新增 Hook 进 `EL.json` + 预检；能反射取地址不写 RVA；不 Hook `IsPlayer`。
- 不为了“看起来对”打勾；计数器的重放成功不算证据。

## 5. 进度

- [x] ADR 0007 + 方法论定稿 —— 2026-10-29 · `docs/adr/0007-replicate-causes.md`
- [x] 管线事实 + 两次判决实验入 kb —— 2026-10-29 · `docs/kb/pipeline-facts.md` §8–§9
- [x] 敌人命令通道端到端 —— 2026-10-29 · `cmds: host 1/1 → client replayed 1`，`localKept fx=1`，commit `30cac60`
- [x] 动画域开关 —— 2026-10-29 · `animLocal`，commit `1209bd8`
- [ ] V2 干净状态验证
- [ ] V3 状态校正
- [ ] V4 交互减法
- [ ] V5 莉莉 / 精灵
- [ ] V6 通道归零

## 6. 历史

- P0–P6（2026-10-25 ～ 10-27）：见各 kb。
- P7-1 / P7-2：两条失败线，见 §0。
- P8（2026-10-29 起）：本文件。
