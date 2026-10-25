# party（本地多人，`native/party` + `native/input`）依赖的事实与 P1 验收记录（v2）

来源标注同 `engine-facts.md`。Dev2 = `Win64_For_Mod_Dev2`，UserDir `ELMP_Test_Data\dev`，测试存档槽 0，map_church_08 椅子旁。引用 v1 结论注明 `v1 party-facts「…」`。

## P1b 输入与座位（v2 2026-10-25）

### 接线与 G0
- 设备接线（`UnrealWindow` 子类化 + 键盘 Raw Input 注册、`XINPUT1_3.dll` 序号 2 导入表项改写）**按需安装**：第一次有设备座位加入或开启静默时才装；进程启动、无座位时三项都未安装（`elmp devices` → `window=false keyboard-raw=false xinput-iat=false`）。装上之后路由器不活跃（没有设备分配给已加入座位、未静默）时，窗口过程与 `XInputGetState` 都原样直通。`[E-实测]`
- 导入表项安装时仍指向系统 `XInputGetState`（未被别的工具改过），实测改写成功；游戏在**后台**也经该导入轮询手柄 0：Dev 模拟手柄 2（归 P1）推左摇杆 → P1 `idle_turn → run → run_stop`，Y −1957 → −2582（经“P1 手柄合并”报给原版的 0 号）。`[E-实测]`
- **性能事实**：`XInputGetState` 读一个未连接的序号约 0.33–0.35 ms（Dev2 实测：座位绑定了未连接的 `xinput:1` 时 `input.pump` 每帧 P50 339 µs；1 Hz 探测三个空序号时 P99 463 µs）。因此 v2：连接探测放到独立线程（1 Hz，原子标志），游戏线程只读已连接的手柄。修复后 P1 + P2（设备）+ P3（脚本）`frame.elmp_us` P50 13 / P95 26 / P99 52 µs，`input.pump` P99 31 µs。`[E-实测]`
- 离开最后一个座位后：路由器回到不活跃（直通），设备分配保留（`xinput:0 -> P1 (assigned P2, seat absent)`），6 s 内 `g0.reflect_calls = 0`。`[E-实测]`

### 键盘归属与吞消息
- Dev2 机器枚举到 4 个键盘（PS/2 内置按完整路径、3 个 USB 按 `vid&pid` 合并）+ XInput 0（真实 Xbox 360 手柄，只读使用）。`[E-实测]`
- 把 `kbd#3` 分配给 P2 后，注入该键盘的 Right 按下 / 抬起（Dev：`elmp kbd`，记一条 Raw Input 事件 + 向游戏窗口 PostMessage WM_KEYDOWN/UP，不经 SendInput、不影响前台程序）：P2 跑动 Y −1041 → −527，两条原版键盘消息被吞（`input.keys_swallowed` +2），P1 不动。`[E-实测]`
- 归 P1 的键盘注入的消息照常交给原版（吞计数不变），但**后台窗口里 PostMessage 的键盘消息不会让 P1 移动**（UE 只处理有焦点时的键盘输入）——这是注入手段的限制，不是路由结论；P1 的键盘路径只以“未被吞”为判据。`[E-实测]`
- 一帧内按下又抬起的键（Dev `kbd … tap`）在 v2 初版会丢失（泵只看当前按住）；已加“每键盘按下锁存”（被座位读取一次后清除），修复后 `J tap` → `jump_up → jump_apex → jump_down → landing`。`[E-实测]`

### 静默 / 绑定 / 持久化
- `elmp mute 1`（`muteInput`，不持久化）：模拟手柄 1（归 P2）与 2（归 P1）推摇杆、P1 键盘按 Z，P1 / P2 位置都不变，键盘消息被吞；分配表不变；`mute 0` 后恢复。`[E-实测]`
- `elmp bind 2 jump key J`：Z 不再跳、J 跳；写入 `Saved/ELMP/ELMP/devices.json`（schema 1，`assignments` + 按动作名的 `bindings`，键名为可读标签）。重启实例后分配与 J 绑定读回，接线仍等到有座位才装。`[E-实测]`

### 座位生命周期
- `elmp join`：P2 生成在 P1 身后（v1 摆位：70 UU × 序号，4 个一轮，抬高 10），档案 = P1 共享进度（能力 / 强化物品 / 精灵等级 / 拥有遗物与槽数 / 等级与祈祷等级）+ 座位自己的默认装备（s5000 两组），恢复后 HP 400/400（P1 的 42 个最大 HP 强化物品镜像过来）；自动分配一个已连接且未分配的手柄（`xinput:0`）并保存。截图 `p1b-three-lilies-dev.png`（P1 + P2 + P3 排开）。`[E-实测]`
- 回标题（teardown）：两个座位随世界丢弃、租约释放、路由器座位清空；重新读档后**不自动重进**（按存档恢复名单需要 saveId，P1e 实现；在此之前 fail closed，避免 A 档的队伍进入 B 档），`registry verify` consistent。`[E-实测]`

### 测试辅助（P1b，登记）
- 读档 ×4、回标题 ×1；Dev 模拟手柄 `fakepad 1/2`（各数次）、键盘注入 `kbd`（kbd#1 / kbd#3，约 10 次）；只写 Dev2 的 `devices.json` 与测试世界；未向用户正在使用的程序发输入（PostMessage 只投到 Dev2 的游戏窗口），真实手柄只读。
