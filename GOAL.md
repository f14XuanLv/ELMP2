# GOAL：P8 统一管线验证 —— 输入驱动的执行层呈现（2026-10-29）

> 本文件是 `/goal` 的完整计划。每轮开始前先读本文件与 §1；计划有变先改本文件（标日期）。
> **这一版刻意收窄**：只回答一个问题——“把 Client 的远程角色放回原版管线（输入 → 命令 → 状态 → 动画），能不能同时拿到 P7-1 的手感和 P6 的完整性”。不铺开覆盖矩阵，不按点位补旧方法论。
> **进度以 §6 勾选表为准**：完成一项打勾并补「日期 · 证据 · commit」。

**当前进行中**：V1 输入驱动实验（设计已定，未动手）
**需要用户**：① 一个带梯子 / 攀爬 / 祈祷点的存档；② 一个入口图里有未开门 / 宝箱 / 可进入未打 Boss 的存档。都只影响对应验证，不阻塞 V1。

## 0. 三条线的结论（用户实机，2026-10-29）

| 线 | 分支 / commit | 机制 | 实机评价 |
|---|---|---|---|
| P6 | `main` 基线 `4972eb0` | 冻结执行层 + 快照驱动姿态 + 事件重放产物 | 完整性最好，手感差 |
| P7-1 | `explore/p7-failed-client-vanilla-execution` @ `a4c49cd` | Client 跑原版执行层 + 复制命令 + 收束结算 | 手感最好，完整性 / 正确性差 |
| P7-2 | `explore/p7-failed-snapshot-coverage` @ `c48d5a5` | 沿用 P6 + 覆盖矩阵逐点补 | 两头都输；目标过宽、不专注 |

用户对方法论的原始判断（2026-10-27，塑造了 P7-1）：**应该找上游统一的集中管线，让 Client 触发，同时剥离威胁 Host 权威的部分（做减法）；而不是凑出和 Host 高仿的加法路线。**

## 1. 调研结论（`docs/kb/pipeline-facts.md`，2026-10-29）

用 IDA MCP + CUE4Parse 查到的结构，把上面的判断落到了具体函数：

- **统一入口是 `AZenithCharacter::Tick`（`0xCF3740`）**：`InputConsume`（`0xCEF310`）→ 输入快照写进命令队列（`0xCBCA20`）→ 输入生成命令 `TryAddCommands`（`0xCC2520`）→ 推进队列 `UpdateCommands`（`0xCC3970`）→ 行走组件 tick（`0xCBAE40`）。
- **命令 = 五条数据时间线**：`State / Ability / Move / FX / Sound`（`CommandAction.h` 字段 = `0xD29F90` 的五个初始化调用）。复制命令 = 五条通道同时到位。
- **动画是推导出来的**：每角色一个 `USpineAnimationComponent`，按层（`LOCOMOTION / ACTION / REACTION / DEATH / EVENT`）从状态组件 + 行走 + 移动 + 输入推导；过渡表按“当前动画已播时长”决定。复制动画名 = 复制输出，必然丢相位 / 过渡 / 层。
- **状态是类**：`UStateComponent` 维护 `UState` 子类集合并广播；命令只是产生状态的路径之一，坐椅 / 攀爬 / 死亡姿态另有入口。
- **减法边界是结构性的**：`OnApplyEffect`（`0xD3BCB0`）/ `OnFinalizeEffect`（`0xD3BD40`）/ `StartDeathProcess`（`0xCBD9A0`）/ `DoDamage`（`0xCA0380`）/ `SaveGame`（`0xD25410`）/ AI 激活（`0xCCD8F0`）/ 交互 Pressed（`0xCE8BF0`），全部在“表现已经生成之后”。

**由此得到 P7-1 的结构性错误**：它以“命令”为单位复制，而不是“输入 + 角色组件栈”。所以 `TryAddCommands` 被抑制、非命令状态缺失、只能 `hold_host_pose` 打补丁；结算又按效果类白名单枚举，未知类 fail closed，覆盖率下降。

## 2. 本阶段假设（要被 V1–V3 证实或证伪）

> 复制 **输入快照** 而不是命令；让远程角色的完整 tick 跑起来；只在 §1 的结算汇点上做减法。手感和完整性会同时成立，且不需要任何“按状态 / 按点位”的补丁。

三个必须验证的分支：
- **H1 输入足够**：`FInputSnapshot`（40 B）逐帧复制后，`TryAddCommands` 在 Client 生成的命令序列与 Host 一致（阈值：同一个 CommandAction 类 + 同一方向，误差 1 帧内）。
- **H2 状态可校正**：长连段 / 受击 / 死亡时副本 `UStateComponent` 可能与 Host 分叉；用“状态类集合”快照校正（而不是动画名）足够收敛。
- **H3 交互可分离**：坐椅 / 祈祷 / 梯子的“状态 + 动画”能留在 Client，而“存档 / 清除 / 治疗”等副作用能减掉。

## 3. 验证步骤（一步一提交，做完一步再决定下一步）

**V1 输入驱动实验（单角色：远程莉莉副本）**
- Host 侧：捕获 `FInputSnapshot`（`+224`，40 B）逐帧发出；先不做命令复制。
- Client 侧：把快照喂进副本的 `InputComponent`，放开 `TryAddCommands`，`UpdateCommands` 正常跑；P6 通道保留但默认关闭，作为回退开关。
- 量：命令序列一致率（类 / 方向 / 帧差）、动画变化沿一致率、位置差、非命令状态（坐椅、死亡姿态）是否自然出现、帧时间。
- 判据：H1 成立；坐椅与死亡姿态**不需要** `hold_host_pose` 就出现。

**V2 状态校正**
- 快照加“状态类集合”（`UStateComponent::ActiveStates` 的类名集合，按 Ref 发）。
- Client 只在“类集合不一致且无进行中命令”时做校正；校正后看是否收敛、是否抖动。
- 判据：H2 成立；连续 60 s 战斗后状态集合一致。

**V3 交互减法**
- CUE4Parse 读 `BP_Interactable_RestingPoint` 的 ubergraph（`OnInteract` → `ExecuteUbergraph[155]`）+ IDA 跟 `AInteractable::OnInteract`（`0xCE8BF0`）→ 状态 / 存档 / 清除的分界。
- 目标：Client 跑交互的“给角色加状态 + 播动画”，减掉存档 / 清除 / 治疗；先只做休息点，再做梯子 / 祈祷。
- 判据：H3 成立；Host / Client 两侧状态一致、Client 存档哈希不变。

**V4 横向推广（只在 V1–V3 全绿后）**
- 同一套接到敌人 / 精灵 / 伴随精灵；P6 通道逐个降级为兜底，不删。
- Boss / 世界对象 / 全局（时间缩放 / BGM）按需，不在本阶段开头做。

## 4. 验收口径（每步都带）

- **手感**：远程对象相邻帧位移跳变（>50 UU 帧数 / 总帧）、动画起始相位（Client 从 t=0 开始的比例）、动作多样性（变化沿总数 + 按序匹配率）、输入到呈现延迟。
- **正确性**：Client 结算泄漏 0（damage / skill / save / effect / death）；两侧敌人数量与 HP 全等；Client `Saved/` 哈希不变。
- **性能**：Host / Client `elmp perf` P99 < 1 ms；无会话 G0 不变。
- **完整性**：P6 的已知点位不得回退（用 P7-2 分支的 `sync-coverage.md` 当检查表，状态重测）。

## 5. 铁律

- P6 通道不删；每个被换掉的域都要有运行时开关（P6 仿造 / 执行层）。
- 放开执行层前先闭合结算汇点，并带计数；未知一律 fail closed。
- 文档先行：原版事实与文档冲突先改 `docs/`。
- 不碰正式游戏目录 / 存档；测前备份实例 `Saved/`，测后还原；用户在用电脑时不做前台自动化。
- 不在 `main` 上直接铺大改：每一步可单独回退。

## 6. 进度

### 6.1 调研
- [x] 统一管线事实（Tick / 五时间线 / 动画单件 / 状态类 / 结算汇点）—— 2026-10-29 · 证据：`docs/kb/pipeline-facts.md`（`[E-IDA]` 0xCF3740 / 0xD29F90 / 0xD24CA0 / 0xCBAE40，`[E-SDK]` CommandAction.h / SpineAnimationComponent.h / StateComponent.h）· commit 本提交

### 6.2 验证
- [ ] V1 输入驱动实验（远程莉莉副本）
- [ ] V2 状态校正
- [ ] V3 交互减法
- [ ] V4 横向推广

### 6.3 收尾
- [ ] 手感 / 正确性 / 性能三项达标
- [ ] 文档 + 版本 + 打包 + 三实例冒烟
- [ ] 阶段报告

## 7. 历史

- P0–P6（2026-10-25 ～ 2026-10-27）：见各 kb。
- P7-1（2026-10-28，`explore/p7-failed-client-vanilla-execution`）：命令级复制 + 收束结算；手感好、完整性差。
- P7-2（2026-10-28 ～ 29，`explore/p7-failed-snapshot-coverage`）：P6 + 覆盖矩阵；两头都输，目标过宽。
- P8（2026-10-29 起，本文件）：输入驱动的统一管线，减法收束。
