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
