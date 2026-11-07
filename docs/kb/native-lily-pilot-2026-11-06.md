# ELMP v2：远端 Lily 原版执行快速试验

2026-11-05 开始，2026-11-06 交付用户游玩。决定见 ADR0010。本记录将构建、原版链观测和完整表现验收分开；未宣称同步已完美。

用户首轮游玩反馈（2026-11-06）：Client 所见 Host Lily 的管线和完整性观感改善，但严重卡顿、同步延迟明显。性能与平滑度验收失败；先提交此版本作为效果基线，再独立排查修复，不能把下述无怪地图短测当作性能通过。

## 实现替换

- 删除远端 Lily 的冻结、Spine Pose/Visual 应用、每快照位置写入；删除 Spirit 裸副本和 Companion 副本适配器，仅保留原版 Companion.FX 的 exact 分类函数。
- 删除 LilyRow 的 Pose/Visual 及整个 sp/cp 快照模型/编解码/Host 采样。敌人保留现有 Pose/Visual 呈现。移除域的旧 codec 用例改为剩余敌人域的容量/键替换/删除与坏包验证。
- 新 `coop/native_lily`：经现有已 Possess 的 ELMP PC/Pawn、原版 profile 库存/装备入口，建立完整原版 Spirit/Companion；Host 在真实 InputConsume 后捕获完整输入、消费序号、Pawn serial、位置/速度/MovementMode、ActiveStates；Client 依可靠顺序调用原版 AZenithCharacter::Tick。每渲染帧最多4份，不丢输入，组件按总时长推进。
- 原版 Ability 及其子时间线/私有粒子只保留一个本地写者，Lily/Spirit/Companion 普通 Act/Fx/Snd 复制停止投递；保留有来源证明的命中反馈。原版对象沿装备/PC 清理。
- 原版 `SetSpineData(FSpineDataGroup)` 替换手工资产替换、SetupPose、强播 idle 与灯光材质设置；按表行1/2/5/7/9/11选择六种外观，FProperty深复制结构。共享镜头用 RegisterAllyController/UnregisterAllyController。

2026-11-07 更正：上条将 Client 远端 Lily 注册进共享镜头违反 F2.7 / C10，是 `0639996` 引入的回归。已移除远端执行模块的镜头注册，恢复 Client P1 单目标及每帧目标表清洗；远近移动验证见 [Client 单人镜头回归修复](client-camera-regression-2026-11-07.md)。下文「已注册共享镜头」仅描述当时观测，不代表独立镜头验收通过。
- Client 伤害、Effect Apply/Finalize、死亡结算、存档闸门继续生效；远端 Pawn 关闭 overlap、屏蔽交互输入，保留世界阻挡。只放行原版 Companion AI 的跟随，不放行敌人 AI。
- 受击 Reaction 路由扩到包括 Host P1 的所有网络可见 Lily；按 seat 去重。此新增远端分支本轮未做自然受击验收。
- 协议9，旧端拒绝。原版高层 CreatePlayer 会增加 LocalPlayer/本地输入/视口语义，本轮远端虚拟座位保留已证 PC/Possess 创建路径，不将它当成任意远端玩家的替换。

## 新取证与首轮修正

`[E-IDA]` 工作库 elmp，AZenithCharacter::Tick VA `0x140CF3740` 完整70行；本轮 get_bytes 前缀 `488BC4564881EC900000000F2970E80F28F148895808`，已加入 NativeLilyCharacterTick 的 SHA/prefix 预检，实机武装成功。

`[E-源码]` UE4.26 Actor.cpp 的 TickActor 使用 CustomTimeDilation；Actor.h:3556 的 FActorComponentTickFunction::ExecuteTickHelper 同样给组件 delta 乘 owner.CustomTimeDilation。因此角色原版输入子步与组件总时长分开推进，避免一帧只消费一次造成积压。

`[E-实测]` 首版一渲染帧只消费一份，约75秒后 FIFO 达48，不能交付为丝滑版本。增加经预检的原版 Tick 逐份推进后，最终运行超过3分钟，Client received=11182、consumed=11182、queued=0；中间读取为2或3，fault空、无坏快照/丢包/身份冲突。不是通过丢弃按下沿清队列。

## 最终构建与实例

- `[E-实测]` Dev/Shipping/FrameProbe 构建通过，7/7 CTest；新 LilyFrame 检查最大全状态、截断、尾随字节、NaN、零序号、超状态容量。
- `[E-实测]` Dev DLL SHA256：`b3a6e15bcd2442570b2db746b3ea0211f1656841f53facfb17c5fc6b7a3f289a`，Host2/Client2 一致。两个包已重新生成，Shipping79项、零Lua。
- `[E-实测]` 最终 Host PID3596、Client PID26196，端口7777，口令native9，Client=P2、map_church_08。收尾两实例保持运行和连接，未恢复/移动/激活窗口，未投递窗口输入。
- `[E-实测]` 操作前备份并校验41个 SaveGames/ELMP 文件；收尾41项 SHA 均未改变。备份 `build/native-lily-pilot/saves-before/`，审计 `save-audit.json`。

## 原版链短测

`[E-实测]` `build/native-lily-pilot/native-smoke.json` 保存22个串行请求与原始返回：

1. 在 Host 的隔离实例通过 elmp pad 临时驱动P1短距离移动、跳跃、攻击。末尾 stop/allow1 off；恢复原召唤组。没有系统键鼠输入。
2. s5060：Host P1及Client远端同时读到 `summon_DEFAULT_C`，原版 Assassin Spirit 皆 Faction=1、Owner=各自对应PC、GetSummoner=对应Lily。采样位置相差约0.65UU；不是逐帧相等结论。
3. 切第二召唤组后 s5040：两侧 `summon_DEFAULT_A`，Witch 的 Owner/Summoner/Faction 均正确；两侧均读到原版 `AbilityHitbox_s5040_AttackA_Projectile`。Client native Ability 3个activation/1个子、私有PSC kept=2，旧Act重放与FX重放均0，unsupported/identityOverflow=0。
4. 结束后 Client 原版装备上下文应用3次（初始、换组、还原），状态修正4次。旧 presentation actors/samples/applied、Visual capture/apply 均0；符合该域删除测试。敌人未在此无怪地图验收。
5. Host/Client 后台截图均1280×720并目视：两只Lily、正常颜色与落地点；已注册共享镜头。文件为各实例 Saved/ELMP/pilot/shots/native-lily-final-{host,client}.png。静态截图不能证明连续相位、技能结束和所有音效。

## 待用户游玩及未覆盖

Host 的移动/跳跃/召唤已经进入新原版链，但尚未把本轮短测当成全游戏验收。Homing 的非空选敌/目标组件速度、Host命中后权威子停止与该新生成族的绑定、晚加入进行中的命令、剧情/椅子/攀爬外部原因、死亡到重生、新 Pawn serial 分支、多地图和所有Boss均须继续验证。完整输入和当前状态本身不保证上述外部原因一致；不以场景无敌人时的弹体存在证明命中结束一致。

性能观测仅为该短场景：frame.elmp_us P99 Host424us、Client173us，包含初始化窗口；新 detour 中原版角色执行耗时不包含在帧泵该指标，不能作为完整每帧增量预算验收。
