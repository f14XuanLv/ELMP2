# GOAL：远程同步覆盖率与正确性（P7 重启，2026-10-27）

> 本文件是 `/goal` 的完整计划。每一轮开始工作前先重读本文件顶部两行与 §0；计划有变先改本文件（标日期）。
> **进度以 §7 的勾选表为准**：每完成一项就在该行打勾并补「日期 · 证据（kb 节 / 截图 / 命令输出）· commit」；「当前进行中」同步更新。goal 完成 = §7 全部打勾。
> 只有在全部完成、或某项经充分尝试仍无法达成（写明尝试、证据、剩余选项）且再无其他可做项时才报告 goal 完成或 blocked。需要我决定的问题：先按需求树已有定案与 fail closed 默认继续其他工作，记到顶部「需要用户」与阶段报告，不要停下等。

**当前进行中**：§4 —— 蓝图直接粒子完成；下一步 §7.4 敌人 VisualPivot 竖直偏移（2026-10-28）
**需要用户**：（无新增）①G3 / 覆盖矩阵里门 / 宝箱 / Boss 行需要一个入口图里有未开门、宝箱、可进入未打 Boss 的存档放到 Host2 / Client2（没有就按同通路代证并在矩阵标「未在真对象上实测」）。

## 背景与本阶段目标
- `main` 已回退到 `4972eb0`（P6，协议 3）。P7（`a06a320..a4c49cd`，“复制决策 + Client 跑原版执行层 + 收束结算”）已整体移到分支 `explore/p7-failed-client-vanilla-execution`，**不接续、不 cherry-pick**。用户结论：Client 表现更丝滑，但**正确性与同步覆盖率都变差**——方向可能对，但很多点位没探索好就一次性全换，导致既没探索好也没实现好。
- 本阶段目标：**把“远程同步覆盖率”做成一张可度量的矩阵，先调研、再逐点实现，每一点都以 Host / Client 对账证据打勾，且正确性（Host 权威、客人零结算、状态收敛、无回归）在每一步都不下降。** 丝滑是次要指标，不能用正确性换。

## 0. 恢复上下文（每轮先读）
1. `AGENTS.md`（规则、资料、证据等级、常用命令，含 `elmp lan host|join|leave|status|trace|bg`）。
2. `README.md`「当前阶段」「已知限制」——P0–P6 全部完成（见本文件 §8 历史）；已知限制就是覆盖矩阵的起点。
3. `docs/REQUIREMENTS-TREE.md` §2.3 F2.*、§3 C14–C18、§4 M17–M24、§7.17（P2）、§7.20（P6）、§8；`docs/ARCHITECTURE-V2.md` §6 Hook 总表、§7.5 coop、§8 协议、§16.1 缓存清单。
4. `docs/kb/net-facts.md`（尤其「P2 首次联机切片」「P6 远程表现补全」及其「遗留」）、`engine-facts.md`、`core-facts.md`、`party-facts.md`。
5. 现有代码：`native/coop/{session,host,client,ability,fx,maps,entity.h,entities/*}`、`native/guard/guard.*`、`native/net/{protocol,snapshot,codec,transport}.*`、`native/console/coop_commands.cpp`、`data/binary/EL.json`。
6. **失败分支（只读，拿知识不拿代码）**：`git show explore/p7-failed-client-vanilla-execution:<path>` 读 `docs/adr/0006-client-vanilla-execution.md`、`docs/kb/engine-facts.md`「CommandQueue 决策管线」、`docs/kb/net-facts.md`「P7 调研 spike」「P7 实施」、`native/guard/guard.cpp`、`native/coop/{commands,client,host}.cpp`、`data/binary/EL.json`。其中的 IDA / 实测事实（命令管线入口、结算汇点表、休眠敌人注入命令的行为、莉莉移动动画由输入驱动等）仍然成立，可按证据等级搬进 `main` 的 kb（注明“来自 explore 分支 2026-10-28 调研”）；其实现与方法论不照搬。需要复现其缺陷时可用 `git worktree add D:\CodeT2\ELMP-p7 explore/p7-failed-client-vanilla-execution` 单独构建部署到 Host2 / Client2 观察一次，观察完 `git worktree remove`，不在该分支上继续开发。
7. 旧仓库知识（只读）：`D:\CodeT\ELMP\docs\kb\net-facts.md`「呈现化接管」「动作通道」「重构后验收缺陷」、`docs/adr/0003-net-coop.md`。

## 1. 复盘 P7（先于一切代码）
产出：`docs/kb/net-facts.md`「P7 复盘」+ `docs/adr/0006-remote-sync-coverage.md`（`main` 上的 ADR 0006 取这个名字；分支上的 ADR 0006 作为“被否决的草案”在其中引用）。
- 用 `git diff 4972eb0 explore/p7-failed-client-vanilla-execution --stat` 与逐文件 diff 列出 P7 改了什么（guard 效果类拦截、命令通道、敌人只停决策源、莉莉副本回带输入 + 本地命令抑制、`hold_host_pose`、删除 Act / Fx / Snd / ActStop / 精灵副本）。
- 对每一项写出：它想解决什么、依据的事实是否仍成立、**为什么正确性 / 覆盖率变差**（已知线索：坐椅子休息等不经命令管线的状态副本到不了，只能打补丁；结算汇点靠“按类放行 4 个表现类 AE”枚举，未知类一律拦截 → 覆盖率反而下降；Client 放开执行层后本地生成的敌人 / 弹体 / 掉落需逐个收养或销毁；连段段号、冷却、着地等本地状态与 Host 不一致导致执行分叉；去掉快照动画同步后失去兜底；一次性删除 P6 通道没有回归基线）。线索要么用分支 kb / 代码证实，要么在 Host2 / Client2 上复现一次证实，不要凭印象写。
- 结论写成“**每个同步点位可选的机制清单**”而不是单一方法论：①快照状态（位置 / HP / 动画名 / 可见）②事件复制（原版入口 pre 取参 → Client 同入口重放：技能 / 特效 / 音效 / 结束 / 受击）③决策复制（命令启动沿 → Client `AddNewImmediateCommand`，仅对“执行层可在 Client 无副作用跑完”的对象 / 命令类）④放开部分执行层（按组件 / 按命令类，前提是结算汇点已用 IDA xref 封闭并有计数）⑤不同步（写入已知限制）。每个点位选哪种，由 §3 的调研证据决定。

## 2. 同步覆盖矩阵（本阶段的验收工具）
文件：`docs/kb/sync-coverage.md`（每行一个同步点位，列：编号 · 类别 · 点位 · Host 源头（原版入口 / 字段，证据标签）· 当前状态（已同步 / 部分 / 缺失 / 不同步）· 机制（§1 的 ①–⑤）· Client 验证方法（命令 / 对账 / 截图）· 最近实测日期 · 备注）。先按下面的清单把行列齐，再在 Host2 / Client2 上**逐行实测当前 P6 的真实状态**（不是按 kb 推断），作为基线。
清单（至少覆盖，可增不可删）：
- **远程莉莉**（Host P1 副本、其他客人副本）：位置 / 朝向 / 速度、全部动作状态（idle / run / jump / fall / land / dodge / 各段 attack / summon / heal 祈祷 / hurt / knockback / death / revive / rest 坐椅 / interact / ladder / hook / 水中 若有）、精灵出现 / 消失 / 技能、弹体、特效（含蓝图直生 `SpawnEmitter*`）、音效（一次性 / 附着 / 循环）、受击闪白 / 顿帧、皮肤 / 装备外观、HP / MP / 祈祷（HUD）、名字标签、死亡与复活、切图 / 跟图淡入淡出。
- **敌人**（出生点 / 动态生成 / Boss）：存在 / 出现 / 死亡 / 消失、位置 / 朝向 / VisualPivot、动画全状态、技能 / 弹体、特效、音效、受击反应、Boss 阶段与 Boss 血条 UI、Boss 死亡慢动作 / 时间缩放、召唤物收养 / 销毁。
- **世界对象**：门、宝箱、可破坏物、拾取物 / 掉落 / 经验球、椅子（点亮 / 休息中）、机关 / 开关 / 升降台、剧情旗标 / 教程 / 对话触发结果、区域名显示、地图解锁、存档点。
- **客人自身**：HP / MP / 祈祷 HUD 与受伤表现、死亡 / 复活流程、升级 / 装备反馈、相机、暂停语义（Host 暂停 / 菜单时 Client 看到什么；Client 暂停时 Host 不受影响）。
- **全局**：时间缩放（hitstop / Boss 慢动作）、BGM / 环境音区域切换、后台图（客人所在、P1 不在）各行的降级状态。
- **正确性行**（每次实测都要带）：Client 结算泄漏计数（`damage / skill / save / effect / death` blocked 之外的放行数必须 0）、Host / Client 敌人数量与 HP 全等、Client 世界清除 / 存档文件哈希不变、死亡 / 复活 / 切图 / 晚加入 / 重连后各行是否仍绿。

## 3. 正确性与对账工具（先做工具，再做功能）
P7 失败的一个直接原因是没有能复跑的回归基线。先补：
- `elmp lan sync [cat]`（两侧都可用）：按矩阵类别输出可对账的摘要——实体表（Ref、类、位置、HP、轨道 0 动画名 + 时间、可见、活跃技能数）、特效 / 音效 / 技能 / 命令计数、guard 全部计数、`Saved/` 关键文件哈希；输出 JSON 行以便脚本 diff。
- `tools/sync_diff.py <hostInst> <clientInst> [cat]`：同一时刻对两实例各跑一次 `elmp lan sync` 并给出差异（位置阈值 5 UU、动画名、HP、计数），结果可直接贴进 kb。
- `tools/scenario.py <name>`：用 bridge 驱动的固定剧本（备份 → 读档 → `lan host/join` → 剧本动作 → `sync_diff` 采样 → 截图两侧 → 还原）。至少 6 个剧本：`slime-fight`（Host P1 在 church_07 史莱姆群连按 ATTACK + 被打）、`guest-fight`（客人座位打敌人 / 被打 / 被击杀）、`world-objects`（可破坏物 / 掉落 / 椅子 / 触发器）、`death-respawn`（Host P1 死亡回椅、客人死亡复活）、`map-change`（跟图、后台图）、`join-reconnect`（晚加入、断线重连后对账）。每个剧本的截图成对命名 `<scenario>-<step>-{host,client}.png`。
- 这些工具本身的验收 = 在 P6 基线上跑一遍，把结果填进矩阵「当前状态」。之后**每个点位实现后都重跑受影响剧本 + 正确性行**，绿才能打勾。

## 4. 逐点调研与实现（按矩阵优先级，一点一提交）
顺序：先正确性缺陷（矩阵基线里任何“正确性行”不绿的）→ 高可见度缺失（附着 / 循环音效、蓝图直生特效、VisualPivot、精灵插值、受击反应、远程莉莉非战斗状态如坐椅 / 祈祷 / 受伤 / 死亡、敌人全动画状态）→ 世界对象与 Boss 行 → 暂停 / 时间缩放 / BGM 等全局行 → 后台图降级行的明确化。
每个点位的工作单元（都要留痕）：
1. **调研**：IDA（`idb_open` 游戏 `.i64`；`elmp native` 取 exec 桩）+ CUE4Parse（`extract.py --dump`）+ 实例探针，定位 Host 源头与 Client 可用的原版入口；若考虑 ③决策复制 / ④放开执行层，必须先列出该对象 / 命令类在 Client 可达的**全部**结算汇点（xref 到底）及拦截与计数，并证明 Client 状态会收敛到快照（不收敛 → 退回 ①②）。结论写 kb（证据标签）。
2. **实现**：沿用现有通道（快照段 / Act / Fx / Snd / ActStop / `Ref`）扩展优先，新增原生 Hook 进 `EL.json` + 预检；协议字段变更升协议版本；有界、幂等、乱序重试沿用 P6。
3. **验证**：重跑受影响剧本 + `sync_diff` + 两侧截图；正确性行全绿；Host / Client `elmp perf` P99 < 1 ms（记录游戏整体 `frame.interval_us`）；两客人（Client3）至少对高可见度点位各跑一次。
4. **登记**：矩阵该行改为「已同步」+ 日期 + 证据；§7 打勾；commit（一点一提交，信息写清点位与机制）。
不允许：为了一个点位整体更换 Client 呈现方法论；删除现有通道却没有矩阵全绿的替代；凭“看起来对”打勾。

## 5. 门槛与收尾
- 覆盖矩阵：所有行有基线与最新实测；「缺失」行为 0；「不同步」行每条都有写入 README 已知限制的理由（技术不可行 / 需要用户存档 / 产品决定）。
- 正确性：6 个剧本全绿；Client 结算泄漏 0；Client `Saved/` 哈希不变；G1–G4、晚加入 / 重连 / 死亡 / 切图 / 两客人 仍绿。
- 性能：Host+1 客人与 Host+2 客人 ELMP P99 < 1 ms，Client < 1 ms；无会话 G0 不变。
- 文档：`sync-coverage.md` 定稿、net-facts 各点位实测、需求树 §7.21「P7 重启实施决定」、ADR 0006、README 已知限制 / 当前阶段、`elmp.json` 版本升级、`build.bat package` 重打 Shipping / Dev 包（零 `.lua` / `.pdb`），Shipping 包三实例冒烟。
- 阶段报告（与 P0–P6 同格式）+ 下一步建议。

## 6. 全程硬规则
- 拿走知识不照搬代码（`D:\CodeT\ELMP`、`D:\CodeT\LilyInEM`、失败分支 只读）；Shipping 零 Lua；游戏线程入口只有 `RegisterEngineTickPreCallback`；缓存优先且每个缓存有失效源与审计计数；无座位无会话时 `g0.reflect_calls` 保持 0。
- 原版管线优先、fail closed、Host 权威铁律（客人进程永不自行结算）、P1 只读；新 Hook 全部进 `EL.json`（前缀 + 来源）并经预检；能反射取地址不写 RVA；不 Hook `0xD150F0`；“没被调用”必须有 detour 计数或 IDA xref。
- 不为测试而测试：验证在 Host2 / Client2 / Client3 真实实例上用剧本脚本 + `sync_diff` + 截图 + `elmp perf` 完成；写世界的测试辅助登记并在报告列出；每轮实测前备份实例 `Saved/`，结束后还原并删除本轮产生的文件；不碰正式游戏目录 / 存档；未经确认不向我正在玩的游戏发输入；实例运行中不替换 DLL；用户在用电脑时不做前台类自动化（见 AGENTS）。
- 文档先行：原版事实与文档冲突 → 先改 `docs/`（日期 + 证据标签）再改代码；每个点位 commit（不 push）；README「当前阶段」按子里程碑更新。
- 需要我介入的事项在报告里单列“需要用户”，然后继续做不受阻塞的部分。

## 7. 进度登记（goal 完成 = 全部打勾）
打勾格式：`- [x] 项目 —— 2026-MM-DD · 证据：<kb 文件#节 / 截图路径 / 命令输出摘要> · commit <hash>`。做不到的改为 `- [!] 项目 —— 原因、尝试、剩余选项`，并同步到顶部「需要用户」。

### 7.1 复盘
- [x] P7 diff 清单 + 逐项失败原因（证实过的）写入 net-facts「P7 复盘」 —— 2026-10-28 · 证据：net-facts「P7 复盘」（7 项 diff + 同剧本 A/B：动画变化沿按序匹配 P6 195/217 vs P7 140/313；P7 史莱姆缺全部 `idle_turn`、攻击循环 7 vs 5，Host P1 副本缺 dodge / jump / damage_*、HP 不更新）；截图 `r7-{p6,p7}-{host,client}.png` · commit 2c53789
- [x] 失败分支里仍成立的事实（命令管线、结算汇点表、注入实验、莉莉输入驱动动画）按证据等级搬进 main 的 engine-facts / net-facts —— 2026-10-28 · 证据：engine-facts「CommandQueue 决策管线」「结算汇点」（`[E-IDA][E-CUE][E-SDK]`，补注“转身 / 走动 / 受击不经命令”“默认 FCommandSettings 插队”）、net-facts「命令注入与输入驱动实验」（`[E-实测]`），均注明来自 explore 分支 · commit 8236589
- [x] `docs/adr/0006-remote-sync-coverage.md`（逐点位机制清单 ①–⑤ + 选择准则 + 否决的草案引用） —— 2026-10-28 · 证据：`docs/adr/0006-remote-sync-coverage.md`（机制表含适用条件与 main 现有载体、5 步选择准则、兜底不删 / 不整体换方法论、验收单元、否决草案）；架构 §18 登记 · commit b218c84

### 7.2 覆盖矩阵
- [x] `docs/kb/sync-coverage.md` 行列齐全（§2 清单全部落行，含正确性行） —— 2026-10-28 · 证据：`docs/kb/sync-coverage.md`（远程莉莉 L01–L30、敌人 E01–E17、世界 W01–W13、客人自身 G01–G08、全局 X01–X05、正确性 C01–C08；新增复盘发现的 L19 伴随精灵、L30 副本动画相位；r7-p6 已证据的行预填）· commit 见本行提交
- [x] P6 基线逐行实测填入「当前状态」（Host2 / Client2） —— 2026-10-28 · 证据：sync-coverage.md 各行「最近实测 = 2026-10-28 基线」（40 行有实测；其余需真对象 / 未覆盖地形的行保持待测并在 §4 对应项实测）；net-facts「P7 重启 · P6 基线六剧本」 · commit 见本行提交

### 7.3 工具
- [x] `elmp lan sync [cat]` 两侧可用，JSON 行输出 —— 2026-10-28 · 证据：`native/coop/sync.{h,cpp}` + host / client `sync_report`（ent / world / fx / cnt / hash，角色无关键 e: d: l: s: x:）与 `elmp lan rec start|dump` 逐帧变化沿记录；Host2 / Client2 church_07 实测两侧 26 实体、world、cnt、hash 正常输出 · commit 见本行提交
- [x] `tools/sync_diff.py` —— 2026-10-28 · 证据：并行取两侧 `elmp lan sync`，按客人座位所在图过滤、5 UU / 动画 / HP / 可见 / fx / world 差异与正确性判据（guard replayed、Host 死 Client 活、Client 本地活敌）；`rec` 模式按实体 LCS（church_07 8 击：84 边 83 匹配）· commit 见本行提交
- [x] `tools/scenario.py` 六个剧本（slime-fight / guest-fight / world-objects / death-respawn / map-change / join-reconnect）在 P6 基线跑通，截图成对入 kb —— 2026-10-28 · 证据：net-facts「P7 重启 · P6 基线六剧本」（六剧本正确性行全绿、rec 79–99%）、截图 `<剧本>-<步>-{host,client}.png` · commit 见本行提交

### 7.4 点位（每行 = 矩阵一行或一组；实现时按实际点位增删行，新增行不删旧行）
- [x] 正确性基线缺陷（基线实测中发现的，逐条列出并修复） —— 2026-10-28 · 证据：net-facts「P7 重启 · P6 基线六剧本」缺陷 ①–⑦ 及各自修复节（① 看门狗 979eb9a / ② 客人死亡姿态 6f8e931 / ③ 复核 / ④ 判定不可见差异 / ⑤ 入座前无副本 / ⑥ 重放预算 03dc683 / ⑦ 重放根组件校验 166b192）；复跑 guest-fight / death-respawn / slime-fight 正确性行全绿、变化沿 93–100%，截图 guest-fight-attack / death-respawn-guest-dead -{host,client}.png · commit 03dc683
- [x] 附着 / 循环音效（`PlayEventAttached` 等） —— 2026-10-28 · 证据：net-facts「P7 重启 · 附着 / 循环音效」（CUE 调用者 4 个、IDA 0x78D7A0、Host 闪避 player-dodge 自然路径与测试辅助史莱姆路径 Client 均附着播放、slime-fight 98% / guest-fight 97% 正确性绿）；矩阵 L24 / E10 已同步 · commit 3c2824f
- [x] 蓝图直生特效（`SpawnEmitter*` / 非 `SpawnFX` 路径） —— 2026-10-28 · 证据：net-facts「P7 重启 · 蓝图直接粒子」（IDA 0x25463A0 / 0x2546230、CUE 调用者清单、Host 捕获 PS_Gloweye / PS_Appear_M 等、Client 重放 fxBad 0、重放预算后 Client P99 453 µs、slime-fight / guest-fight 正确性绿；截图 emitter-slime-{host,client}）· commit 19905f4
- [ ] 敌人 VisualPivot 竖直偏移
- [ ] 精灵位置插值 / 精灵全状态
- [ ] 敌人受击反应 / 全动画状态对齐
- [ ] 远程莉莉非战斗状态（坐椅 / 祈祷 / 受伤 / 击退 / 死亡 / 复活 / 交互 / 爬梯等）
- [ ] 远程莉莉战斗状态全覆盖（各段 attack / summon / dodge / 技能结束）
- [ ] 世界对象：门 / 宝箱 / 可破坏物 / 掉落 / 椅子 / 机关 / 触发结果（真对象实测；缺存档的标「需要用户」）
- [ ] Boss：阶段 / 血条 UI / 慢动作 / 死亡 → 奖励（Host 图内）
- [ ] 客人自身 HUD / 受伤表现 / 死亡复活流程 / 暂停语义
- [ ] 全局：时间缩放 / BGM 与环境音区域切换
- [ ] 后台图各行的降级状态明确化（能同步的同步，不能的写已知限制）
- [ ] 两客人（Client3）高可见度点位复测

### 7.5 门槛与收尾
- [ ] 六剧本全绿 + 正确性行全绿（泄漏 0、哈希不变、敌人数量 / HP 全等）
- [ ] G1–G4、晚加入 / 重连 / 死亡 / 切图 / 两客人 回归
- [ ] 性能（Host+1 / Host+2 / Client P99 < 1 ms；G0 不变）
- [ ] 文档：sync-coverage 定稿、需求树 §7.21、ADR 0006、README 已知限制 / 当前阶段
- [ ] 版本升级 + 重打 Shipping / Dev 包（零 .lua / .pdb）+ 三实例冒烟
- [ ] 阶段报告

## 8. 历史（已完成，2026-10-25 ～ 2026-10-27；证据见各 kb 与 commit）
- P0 骨架（`488863f`）、P1a–P1f 本地多人（`768c154`…`f611b5e`）、P1 遗留（pickup / 精灵升级 / 敌人激活入口 `0xCCD8F0`）。
- P2 同图联机（net / coop / guard / 动作通道 / 远程座位 / 归属 / 装备通路 / 联机页 / 版本一致；G1–G4、晚加入、重连、PvP、§7.2 两列、性能）。
- P3 多客人 + 多地图（Client3、多对端、level_instance / map_context、host/maps、Boss fail closed、12 读者分类）。
- P4 猎人模式 + SDK；P5 发布 0.3.0（README / INSTALL / ADR 0005 / 包 / 三实例冒烟）。
- P6 远程表现补全（协议 3：Ref、精灵呈现、特效 / 音效 / 技能结束 / 受击通道）——`4972eb0`，本阶段的基线。
- P7（分支 `explore/p7-failed-client-vanilla-execution`）：复制决策 + 跑原版执行层 + 收束结算；未采纳，见 §1 复盘。
