# ELMP v2 莉莉管线回归与修复验收（2026-11-04）

基线 `e8a1fe7`，协议6；用户明确未通过：Client本机击退、伊莱恩普攻魔法球与远程技能特效。方法论遵循 [ADR 0009](../adr/0009-restore-native-lily-pipelines.md) 及旧仓库 ADR 0002。

## 基线实测

`[E-实测]` Host2/Client2启动前整体备份SaveGames及Saved/ELMP/ELMP至`build/lily-revision/saves-*`，路径清单`backup.json`。仅背景UEBridge操作隔离实例，无窗口激活/移动/系统输入。

- church_07，Host把网络P2移至Slime21附近，怪物自行攻击；按帧记录双方原版Knockback/Spine/速度/HP，未人工调用受击或动画。Host P2从400降至386，20个已返回采样帧处于原版击退（18帧`damage_launch_neutral`、2帧`damage_launch_down`）；Client自己HP同为386，**0帧击退，0帧damage动画**，主要保持idle并出现jump/landing。证据：`build/lily-revision/baseline-hit-{host,client}.json`。返回序列受UEBridge表序列化数量上限限制，未把遗漏帧视为不存在；在共同有效时间窗中存在明确断链。
- Host默认第一套ATTACK两次，实际为s5060；两侧都有Projectile Ability，但Client emitter抑制计数增加2，Host exact attachment拒绝增加2。文件历史名`baseline-elaine-*`不代表伊莱恩，后续已纠正。
- 伊莱恩s5040：通过原版SpiritEquipComponent.SwitchSummonSet(2)切Host第二套，原版SUMMON_1两次。Host有效`AbilityHitbox_s5040_AttackA_Projectile`记录93帧，Client191帧；两侧本地原版Ability均存在，Client数量/寿命合计异常，正在核父容器与子Ability重复复制。证据：`baseline-s5040-*`；该帧计数不是持续时间或最终因果证明。

测试辅助：LoopInGameThreadAfterFrames每帧读取真实游戏对象，采样240帧后Cancel；移动P2仅为建立怪物攻击场景。原版技能由elmp输入辅助触发；mute/allow1在finally关闭。测试结束须停止实例并还原备份，重新正常启动联机。

## 工具修正

UEBridge在Windows上出现一次`response write failed: File exists`：命令已执行，旧response轮询读取与Lua remove+rename存在竞争。串行请求客户端现于发布下一request前删除上一response，避免读取旧文件阻碍替换；超时不自动重发有副作用命令。

## 验收状态

### 首轮协议7实机（2026-11-04晚至11-05）

Dev DLL `545ebcc03424d490…`；这一轮尚未包含ChildStop及位置到位读回修正，不是最终部署。

- s5040两次原版召唤：Host发送2个根Act、Client重放2个根，两端各由原版派生2个Projectile；单时刻均最多1个Projectile和1个`PS_S5040_A`，Client该来源emitter阻断为0。完整240帧`v7-s5040-*`，逐帧对象关系验证私有Sphere挂点；不再出现基线的双球。背景截图`lily-native-s5040-v7-{host,client}.png`已人工查看，两侧均可见橙色魔法球。截图不证明全部时序、追踪或技能覆盖。
- s2022箭尾`PS_ArrowTrail`、s5060弹体`PS_S5060_A_02`分别有双端原版PSC记录，Client Act/Fx bad均0；帧数不同不可直接当寿命差。证据`v7-s2022-*`、`v7-s5060-*`。
- 自然Slime21攻击：Client自己的damage_launch_neutral、damage_launch_down、damage_start/loop及恢复重新出现。前两次受击双方neutral均18个采样帧；down为Host2/Client3帧。后续第9次Host反应触发Client位置设置返回false而安全离会，故此版连续受击**未通过**。
- 上述位置失败已查原生返回语义：SetActorLocation返回是否实际移动，微小位差或已到位可返回false；新版以实际GetActorLocation逐轴绝对误差≤0.01UU验证，拒绝真实未到位，单列anchorNoMoveAccepted。仍需延长实测，不以源码修正代替验收。
- 两侧首轮Homing目标均NULL，只证明该次无目标路径，不证明选敌一致性。已核选敌/目标速度/初始旋转上下文，见[Homing图谱](lily-homing-context-2026-11-04.md)。

### 后续回归

`[E-实测]` 2026-11-05，Dev `4afa3c0c25866463…`（v7d）：

- s5040两次召唤：双方240帧，单时刻Projectile和PS_S5040_A各最多1；Host ChildStop=2，Client stopped=2、failed=0，unsupported/lookupConflicts=0。根生命周期live=0；内部最后一族的已结束记录可保留到下一启动剪枝，不是仍运行的Ability。截图`lily-native-s5040-v7d-{host,client}.png`已查看，双端魔法球可见，画面各2只莉莉。
- 自然Slime21连续攻击：7段×240帧（双方各1680帧）；分段恢复Host P2生命防止测试死亡，不人工触发击退。第7段结束Host captured=sent=45，Client replayed=45、rejected=anchorFailed=0，anchorNoMoveAccepted=20，连接持续。双方均出现neutral/up/down及恢复；常见完整击退段Host20帧、Client21帧，不能称逐帧相同。后续场景操作累计69次仍无反应失败。
- s2022延迟连发：双方原版ArrowTrail均存在，累计Host ChildStop15、Client stopped9/alreadyEnded6/failed0，精确路径unsupported=0，涵盖原版先结束的幂等分支。s5060双端各25个有效粒子采样帧，Act/Fx失败0。
- Client自己ATTACK可产生原版弹体，Host亦观察到攻击。但其本地预测Ability尚未与Host activation关联，本轮不宣称own-seat弹体命中结束/轨迹已一致；该采样Host2帧、Client26帧的差异需单独闭合，不能作为通过。
- 生存中离会：Client退出前IsActive Ability=8，退出后0，返回标题且session=none。地图church_08↔church_07跟随期间无额外莉莉、无身份冲突。证据`v7d-leave-active-requests.json`及各回归status。
- 测试存档于停止两进程后再次整体恢复并逐文件字节验证；仅隔离实例。最终构建额外增加无会话/无身份时绕过native Launch观察的G0快路，不改变联网分支；Dev/Shipping及7项CTest通过，Shipping包79项且零Lua。最终部署哈希`b9d4cd0d2008800e…`，Host/Client一致。收尾修正`elmp status`会话字段写死none的诊断错误，改读实际coop角色；连接验收同时读取`elmp lan status`，不依靠这个旧常量。

结论：本轮原版击退执行、原版Ability私有FX、去重与子Stop身份链已获得上述范围的真实证据；**完整莉莉同步仍未验收通过**。追踪选敌/Invoker阵营、own-seat预测技能权威关联、HitStop/独立色闪与音效、Summoner/Spirit完整上下文、晚加入活动Ability等保留未闭合。不能因构建、计数或一个技能恢复宣称全部莉莉、Spirit、Notify或Boss管线完整。

### 继续恢复独立受击反馈（协议8进行中）

协议7最终部署后新增基线：`feedback-baseline-v7-{host,client}.json`，自然Slime21攻击240帧，Host P2的DamageColorCurve有76帧IsPlaying=true，Client自己为0帧。双方该次HitStop均false，故这条接触伤害样本能证明色闪缺失，不能用于断言HitStop缺失或修复成功。已增加原版HitStop、Spine timeScale、Movement tick、Color.GetCurrentColor及DamageColorCurve位置的逐帧只读采样。

协议8将Knockback、HitStop和DamageColor实际启动事实按共享native entry_order排序，再赋同一wire seq；仅Knockback更新运动锚点。构建/实机结论待本轮填入。

`[E-实测]` 首版协议8 Dev `8103222b266746d0…`，全构建及7/7 CTest通过：

- `v8-hitstop-restart-*`：义肢只在Host P2调用原版LaunchHitStop(0.35)，随后再次调用(0.1)，不在Client注入暂停或动画。Host captured2、Client replayed2；双方240帧中均27帧IsInHitStop=true、Spine timeScale=0、Movement tick=false，其余213帧原版恢复。此为原版入口与重复启动语义实测，不冒充自然Slime接触会产生HitStop。
- `feedback-v8-color-*`：自然Slime接触伤害，Host曲线72个采样帧、Client62帧运行；Client实际Color.GetCurrentColor有与Host相同的曲线颜色值，结束回白。窗口/时相差异保留，未声称逐帧相等。
- 为稳定对账，完成采样后义肢关闭Host该类敌人的碰撞/tick，停止新的伤害后读取：Host共发送59个Reaction（31次Knockback/HitStop、28次DamageColor），Client received59、Color replayed28、failed/rejected/anchorFailed均0。证据`v8-feedback-stable.txt`和对应请求记录。随后停止两实例，待下一部署恢复原始备份；该辅助不进发布包。
- 音光来源修复仍在实现，本组DLL没有这项变更；其后验收应单独记录。
