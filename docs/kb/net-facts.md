# 联机（`native/net` + `native/coop`）依赖的事实与 P2 验收记录（v2）

来源标注同 `engine-facts.md`。Host2 = `Win64_LAN_Host2`（UserDir `ELMP_Test_Data\host`），Client2 = `Win64_LAN_Client2`（`ELMP_Test_Data\client`），同机双实例、回环地址。引用 v1 结论注明 `v1 net-facts「…」`。

## 传输层（v2 2026-10-26）

### 实现（重写，语义沿用 v1 net-facts「传输选型」「安全加固记录」）
- `net/crypto`：CNG 一次性打开算法句柄；SHA-256 / HMAC 用 `BCryptHash`，HKDF 按 RFC 5869 自写；ECDH P-256 导入对端点失败 → `nullopt`（不抛）；AES-256-GCM `seal/open` 写入调用方缓冲。
- `net/transport` `Link`（单对端、IO 无关）：包头 20 B（magic `EL` / proto / kind / sid / seq，作 AAD）；握手 HELLO（身份 49 B + nonce + 公钥）→ HELLO_ACK（nonce + 公钥 + Host 证明）→ AUTH（Client 证明）→ Host 首个加密包即“已建立”。主密钥 = HKDF(Z, salt = SHA-256(归一化配对码), info = "ELMP v2 master" ‖ SHA-256(transcript))；双向密钥 / IV / 证明键 / sid 由主密钥派生。配对码错 → Client 校验 Host 证明失败，本地 `Rejected(Auth)`；Host 校验 Client 证明失败 → 明文 REJECT Auth。身份不一致 → Host 明文 REJECT（Proto / Exe / Dll / Content），原因送达 Client。
- 可靠通道：每段 ≤ 1100 B 独立序号、`last` 位拼消息（单消息 ≤ 64 KB）；ACK = 连续基 + 64 位位图，只在变化 / 收到重复段时写；只发窗口（已确认基 + 64）内的段；RTO = clamp(1.5 × SRTT, 0.1, 2 s)，每次重发 ×1.5（上限 2 s），Karn 只用未重发段采样；有未确认段且 8 s 无确认进展 → 断开；8 s 无任何入包 → 断开；1 s PING。
- 不可靠通道：每流只保留最新（未发完即被替换），1100 B 分片，重组组 ≤ 32、2 s 过期，旧序号计 stale。
- 上限：待发可靠消息 256、出站 512 包 / 链路、事件 512（先丢不可靠）、PONG 4、每次 poll ≤ 64 包；`F_CLOSE` 连发两次。
- `net/endpoint`：单 UDP socket + 网络线程（`SO_RCVTIMEO` 2 ms，关 `SIO_UDP_CONNRESET`）；对端表按地址；新地址的 HELLO 在活跃链路 ≥ `max_peers` 时回明文 REJECT Full；对端 id 单调不复用；同地址已关闭的链路收到新 HELLO 即重建（重连）。游戏线程只经两个有界互斥队列（命令 1024、收件箱 512，满时先丢不可靠）交互。
- 身份（FR-NET-8）：proto = `elmp.json` protocol；exe = 预检 SHA-256 前 16 B；dll = `main.dll` 文件 SHA-256 前 16 B；data = `Mods/ELMP/data` 下全部文件（相对路径 + 内容哈希，按路径排序）的 SHA-256 前 16 B。会话首次启动时计算（冷路径，75 个文件）。

### CTest（§13 只此三类）
- `test_net_codec`：每个截断前缀都失败、写溢出锁存不越界、varint > 32 位 / > 5 字节拒绝、字符串上限与谎报长度拒绝。
- `test_net_transport`：20% 丢包 + 20% 重复 + 乱序下 120 条（含 5000 B 多段）可靠消息按序恰好一次；不可靠只交付最新且分片重组；256 条上限与 64 KB 上限；断网 10 s 关闭；`F_CLOSE` 即时送达；空闲 20 s 链路保持。
- `test_net_handshake`：配对码归一化（大小写 / 横线 / 空格）后连通；错码 → Client `Rejected(Auth)`；协议 / EXE / 数据哈希不一致 → 对应原因；垃圾包 / 截断 / 非曲线点计坏包且不影响随后正常握手；10 s 握手超时。

### 实测（Host2 / Client2，回环）
- `elmp lan host 4821 7777`；Client `elmp lan join 127.0.0.1 1111 7777`（错码）→ Client `session: none (last: pairing code mismatch)`，Host 侧该链路停在 authing、10 s 后 `handshake timeout` 移除。`[E-实测]`
- 正确码 → 两侧 `connected`，RTT 1.1 ms；Host 同时持有两条链路（陈旧的 authing + 已连接）验证对端表。`[E-实测]`
- Client `elmp lan leave` → Host 1 s 内 `peer 2 closed: peer closed: left the session`（`F_CLOSE`），`peers=0`。`[E-实测]`
- 身份日志：`proto 2 exe 9b939a44 dll 0199bea7 data 78e12c94 (75 files)`，两实例部署同一构建，一致。

## P2 首次联机切片（v2 2026-10-26，Host2 / Client2，两侧槽 0 church_08）
- Client `elmp lan join` → Host 落座 P2（`network`），Welcome epoch 2；5 s 内 Host seq 319 / acked 318，Client applied 313、badSnaps 0、mapReady 一致；guard 已武装。`[E-实测]`
- **G2 取证**：Client 端 `elmp pad allow1 on` + `elmp move 1 1 0`（测试辅助，驱动 Client 本地 P1 的 InputSnapshot）→ Host P2 `idle → run`（Y −2027 → −1692 → −1236 → −871 → −474，原版移动管线，非瞬移）；`move 1 0 0` + `press 1 JUMP` → Host P2 `jump_down → idle`（按下计数差 → `pad::tap`）。`[E-实测]`
- **跟图 + G1 首测**（同日）：Host P1 传送进 church_07 → Client 原版 `SwitchGameMap`（`DT_GameMapData` 行 + Host `WorldLoaderSubsystem.PlayerStartTag`）跟到 church_07、回 MapReady；Client 接管 24 敌（guard marked 48 = 敌人 + 控制器）。初版只拦登记对象时 Client 24/24 控制器 `IsActive`（敌人随生成即激活，先于接管）；改为武装期间 Client 全部 `AZenithAIController::Activate` fail closed 后：Client `aiActivateBlocked=24`、`IsActive` 0/26，Host 24/26。`[E-实测]`
- **G3 敌人首测**（同日，church_07，均静止）：两侧 24 个出生点名一一对应，位置差 ≤ 1 UU（Slime21 6986 / 6985），HP 全等（100 / 40 / 150）。Spine 姿态 / 战斗中位移 / 击杀待测。`[E-实测]`
- **Spine 姿态**（同日）：Client 接管敌人后仍 tick 的组件 = ZenithCharacterMovement / Widget / SpineSkeletonAnimationEx / SpineAnimationComponent / SpineSkeletonRenderer / LightEmulator；其中 `SpineAnimationComponent`（BP 状态机）每帧覆写轨道 0，导致 `SetAnimation` 成功（animSets 16、fail 0）但画面仍 idle。接管时按白名单逐组件关 tick（保留 SpineSkeletonAnimationEx / Renderer / Ability / BoneFollower / Bone / Particle / Widget）后：Slime21 两侧逐次采样 `idle → walk` 同步，Y 7144/7143 → 7064/7063（采样间隔内移动造成的偶发差值除外 ≤ 1 UU）。`[E-实测]`
- **Host P1 副本**（同日，church_08）：Client 以 slot 51（50 + Host 座位 1）生成 ELMP Lily，标记 + 关碰撞 + IsPlayer=false，并按同一白名单冻结组件（不冻时其自身动画逻辑把轨道 0 拉回 idle）。Host P1 `run → jump_up → idle` 时副本 `run → jump_apex → idle`；静止时位置全等（Y −2807.3），移动中差值 = 两次采样间隔内的移动量；HP 400/400 与 Host 一致。Client 本地 P1 = 预测的“自己”，偏离 Host 座位 > 150 UU 时回拉（P2 决定：预测 + 回拉；隐藏锚点留作后备）。`[E-实测]`
- **客人战斗 / 敌人死亡呈现**（同日，church_07）：Client 连按 ATTACK（输入上行）→ Host P2 原版攻击 / 召唤（`summon_DEFAULT_A`），Host 结算击杀 Slime5 / 6 / 9；Client 侧这三只 HP 0 且 `bHidden`（Host 死亡姿态随行下发，行消失后隐藏 + 关碰撞），其余 21 只位置 / HP 两侧一致；Client `kills=3`，guard `damage blocked=9`（Client 本地敌人对本地 P1 的命中全部拦下），Client 自身 HP 取 Host 座位值（250/400 两侧一致）。尝试过的错误做法：Client 回放窗口里走原版 `DoDamage` 让本地敌人死亡 → 敌人停在 HP 1（原生入口对被冻结敌人不生效），且原版死亡管线会清 Client 出生点（v1 缺陷 1），已删除。`[E-实测]`
- **动作通道**（同日，church_07，史莱姆群攻击 Host P2）：Host 原生 detour `AbilityLaunchFromDesc`（`0xCAD0C0`，pre 取参，反射偏移读 `FAbilityDescription` 白名单字段 + 调用的 `Offset`）`captured=38 actsSent=38 dropped=0`；Client 同一原版 `LaunchAbilityFromDescription` 重放 `replayed=38 rejected=0`（`AbilityClass` 经 `IsChildOf(/Script/Zenith.Ability)` 校验，按敌人 seq 幂等）；重放出的命中盒打到 Client 本地 P1 被结算拦截（guard `skill blocked=2`），Client 不结算。`[E-实测]`
- **guard 逐项**（同日 Client2）：`SaveSubsystem:SaveGame()` 返回 false、`saveBlocked=1`、SaveGames 目录最新文件时间不变；`UActorBPFLibrary::IsPlayer`（跳板 detour）对本地 P1 = true、对 Host P1 副本 = false（`isPlayerFalse` 计数）；DoDamage 拦截 20 次 / 技能伤害拦截 2 次（Client 本地敌人与重放命中盒打本地 P1）；AI 激活拦截 24；tick 原语闸 `tickBlocked=7`（接管后原版试图重新开 tick 的被拦）。回放窗口机制保留（`open_replay`），P2 死亡改为纯呈现后暂无调用方。`[E-实测]`
- **晚加入 + G1 复检**（同日）：Host 先进 church_07，Client 之后才 `lan join`（Client 仍在 church_08）→ 名字表重放、Welcome、跟图 church_07、接管 24 敌、Host P1 副本出现；18 s 内 Client `rechecks=20 aiLeaks=0`（每秒读两标志：`ZenithAIController.bActive` 0/26；`ZenithCharacter.bActivated` 24/24 为原版生成即置位、仅呈现），guard `aiActivateBlocked=24`、`tickBlocked` 可观测；两侧 24 敌位置 / HP 全等（静止）。`[E-实测]`
- **非主动断线重连**（同日）：直接结束 Client 进程（无 F_CLOSE）→ Host 8 s 超时 `peers=0 parked=1`，`elmp seats` 仍有 `P2: network … pos=(8600,-429)`（座位、控制器与装备原样保留）；Client 重启读档后再 `lan join` → Build 带同一 guestKey `5ba733…` → Host 日志 reclaimed，新对端 2 仍落 P2（同一 pawn，位置 / HP / 动画计时连续），epoch 2 → 3，Client 跟图、接管 24、rechecks 20 / aiLeaks 0。`[E-实测]`
- **Host P1 死亡重生不误判**（同日）：Host 在 church_07 对 P1 原版 `DoDamage(99999)` → 原版死亡 → 椅子重生到 church_08；Client 跟图到 church_08，`kills=0`（church_07 的行随卸载消失但没有 dead 标志 → 行驻留、不当死亡）。Host 再进 church_07 → Client 跟回，24/24 敌位置 / HP 与 Host 全等、无隐藏；出生点清除数两侧均 1/25（存档原状，Client 未被置清）；`aiActivateBlocked=48`（两次进图各 24）、aiLeaks 0。`[E-实测]`
- **性能（Host + 1 客人，church_07 24 敌，60 FPS，10 s `elmp perf`）**：Host `frame.elmp_us` mean 206 / P50 191 / P95 271 / **P99 426 µs**（其中 `frame.coop_us` P99 398、`coop.host.capture_us` P99 311：每帧 24 敌 + 3 lily 的 Spine 轨道 0 经 ProcessEvent 读取，reflect.calls 主要来源）；Client `frame.elmp_us` mean 66 / **P99 147 µs**（`coop.client.apply_us` P99 120）。两侧均 < 1.0 ms。无会话 / 无座位的 idle tick：`g0.reflect_calls = 0`（Host 1007、Client 1004 个 idle tick），对裸 UE4SS 的 P99 差沿用 party-facts「P1f 帧时间基线」（会话关闭时联机代码只有 `coop` 阶段谓词一次读，不调用反射）。优化余地：Spine 名字按 TrackEntry 指针缓存（§4 第二阶段）。`[E-实测]`
- **worldobj（可破坏物）**（同日，church_07）：Host 原版 `DoDamage` 打碎 `BP_Breakable_Flower_Red4`（HP 0）→ 2 Hz 采样带 broken 位 → Client 该花 `bHidden` + 关碰撞，另外两件（白花、`BP_BreakableBlock_Spine_PileAttack_Lv3_2`）两侧不变。踩坑：(1) 先用 `IsCleared` 当“已破”→ PileAttack 方块在 Host 存档里已清除但仍可见、Client 被误隐藏 → 改为独立 broken 位（HP ≤ 0）；(2) 对 `ABreakable` 读了 `ZenithCharacter.HPComponent` 偏移 → 两个进程都访问违例 `0x10000000b`（dbghelp 符号化：`host.cpp capture_world → PropRef::object → ue::alive`）→ 改用 `Breakable.HPComponent`。Client 不回放原版破坏（会本地生成掉落），只隐藏。交互物（门 / 宝箱）走原版 `OnClearedStatusChecked(true)`，本图无交互物，待 church_08 / 其它图实测。`[E-实测]`
- **客人主动离开**（同日）：Client `elmp lan leave`（F_CLOSE）→ Host 立即 `peers=0 parked=0`、`elmp seats` = no seats（主动离开删座位、释放租约），Host P1 原样（hp 400/400、mp 0/100、heals 3/3、位置 / 动画连续），镜头 `allies=0 registered=0`（远程座位本来就不进共享镜头）。Client 侧 guard 解除、副本删除，但本地敌人仍 `IsActive` 0/26（呈现态世界）→ P2 决定：Client 离开会话后 fail closed 回标题（`OpenLevel TitleMap`），不在呈现态世界上继续单机。`[E-实测]`
- **联机 PvP**（同日，church_08）：Host `elmp team 2 enemy`（P2 = 网络座位），Client 输入驱动 P2 面向 P1 连按 ATTACK → Host `pvp`：`flips=24 scales=21`，`pvpDamage 0.10` 下 P1 400 → 306；Client 上 Host P1 副本 HP 同步 306/400。`set pvpKnockback 2` 后再打 6 次：`knocks=7`，P1 被击退 Y −1957 → −1773、HP 276。命中门 / 倍率 / 击退全部在 Host 结算，Client 只呈现。踩坑：Lily 的精灵发动技能时动作通道对非 ZenithCharacter 读 `SourceSpawnPoint` → Host 访问违例（已修：先 `IsChildOf(ZenithCharacter)`）。`[E-实测]`
- **500 ms 停滞松键**（同日）：Client `elmp hold 1 ATTACK` → Host P2 `raw=ATTACK`；直接结束 Client 进程 1.5 s 后 Host 对端仍 connected（8 s 超时前）但 `input=stalled`、P2 `raw=-`（按键已松，不会卡住连发）。跟图闸门：Host 每个对端记 `mapReady`（= Client 回报已到达的 Host 地图 id），Map 消息只在 Host 地图变化时发、Client 只在本地加载空闲且就绪时调原版 SwitchGameMap。`[E-实测]`
