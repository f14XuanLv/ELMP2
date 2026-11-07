# ELMP v2：Client 单人镜头回归修复

日期：2026-11-07。对应 F2.7 / C10；实现约束见 ARCHITECTURE-V2 §7.4。

## 引入点与修复

代码审计：`0639996ff505fa358484ff81d7f434496defc705`（2026-11-06「恢复远端莉莉原版执行管线并修复播放卡顿」）新增 `native_lily::bind` 对 Client 远端 Lily 控制器的 `RegisterAllyController` 调用，令原版相机同时取景本地 P1 和 Host 副本。HEAD `6d46183` 仍含该调用。未做旧提交二进制 bisect；引入点由新增调用的 diff / blame 确定。

删除远端执行模块的镜头注册、注销及 camera 缓存；保留其控制器句柄供原有因果身份解析。`party/camera_sync` 在 Client 会话每帧直读当前 P1 注册镜头，只保留 `PlayerControllerTargets` 中唯一真实 P1，原地压缩弱指针数组，清空 Ally / Enemy / Other；不分配、不释放引擎数组内存。关闭 Client 共享放宽；Host 的放宽开关仅计本地座位，Network 座位不计入。

镜头与目标不跨帧新增缓存；每帧从当前 PC/manager 解引用，换图后跟随新镜头，离开 Client 角色后停止清洗。`elmp camera` 新增 players/enemies/others/client_solo/client_targets_removed 读回。原版控制器负责 P1 注册和前视、平滑、房间边界。

资料交叉核对：旧仓库 `D:\CodeT\ELMP\docs\REQUIREMENTS.md` FR-NET-3c、`docs/kb/net-facts.md`「视角」记载单目标前视及目标表污染；仅引用事实，未复制旧实现。重建 `Zenith/Public/GameplayCamera.h` 的四组目标均为弱指针数组（`[E-SDK]`，仅证明布局声明）。

## Host2 / Client2 验证

`[E-实测]` `cmd /c build.bat test`：Dev / Shipping / FrameProbe 构建及原有 7/7 CTest 通过；未增加镜头 mock 测试。两个实例停止时部署相同 Dev DLL：SHA256 `bd10a8ad921186461d366f8fda6b692e2e051fcae7971cd215faef58068c5188`。

隔离实例从 slot0 读档进入 `map_church_08`，UDP 7777 联机，Client 为 P2。Host 经 `elmp pad allow1 on` / move / stop 走远及返回；未使用窗口输入。Client 保持原地。串行 UEBridge 原始记录在 `build/client-camera-regression/{connect,status,run,extra,local}.json`，数值复核在 `summary.json`。

| 场景 | Client 所见两只 Lily 的 Y 间距（UU） | Client 镜头 X | Client 取景框 Y / Z 尺寸 |
|---|---:|---:|---:|
| 初始 | 70.00 | -880 | 260 / 260 |
| Host 向右走远 | 1692.13 | -880 | 260 / 260 |
| Host 返回 | 281.26 | -880 | 260 / 260 |
| 再次走远、截图 | 1850.00 | -880 | 260 / 260 |

`[E-实测]` 前三组 Client camera/min/max 逐轴保持一致；第四组镜头 Y 仅浮点差 0.000122 UU，X/Z 不变。远端 Lily 持续消费原版输入，仍可正常走远，并非隐藏或冻结远端来规避镜头问题。各组 Client 均 players=1、allies/enemies/others=0、framing=false、widened=0。

`[E-实测]` 测试义肢向 Client Ally / Enemy 各注册一次远端控制器，随后读回两表均0、client_targets_removed=2，未残留撑远目标。该请求在引擎帧中注册，可能先影响当帧原版更新；0.5s 后采样镜头 X=-880.03284、框已恢复，后续回到 -880.00031。该项证明下一轮清洗生效，不宣称对帧中注入零瞬态。

`[E-实测]` Client 自己向右 / 左移动后，镜头继续跟随；停稳读回 camera.Y - localP1.Y 分别约 +100 / -100 UU，保留原版单人前视。Host 加入本地 script P3 后 allies=1、registered=1、framing=true；Client 同时仍只有 P1。P3 离开，Host allies=0、registered=0、framing=false（只剩远程 P2），本地共享镜头未被破坏。

`[E-实测]` 后台 PrintWindow 成功获取1280×720截图并目视检查：Client `camera-solo-near-client.png` 与 `camera-solo-far-client.png` 的固定本地背景和 Lily 尺寸相同，远处 Host 不再被强行同框；Host `camera-solo-far-host.png` 正常独立跟随。文件位于各配置实例 `Saved/ELMP/pilot/shots/`。未恢复、移动、激活窗口，也未操作系统光标。

`[E-实测]` `r.VSync 0`、`t.MaxFPS 60` 后重置 frame 指标并短测：Client 单人目标清洗 mean 4.893µs / P99 15µs，frame.elmp_us P99 275µs，帧间隔 P50 16.668ms / P99 17.568ms；Host 帧间隔 P50 16.667ms / P99 18.621ms。frame.exceptions=0、G0 reflect calls/lookups=0。此为无怪地图短测，不作为全游戏性能或战斗验收。

## 收尾

测试辅助：slot0读档、Host/Client P1 虚拟轴输入、Host recall=0、本地 script P3 加入/离开、两次原版目标注册注入、关闭VSync及60fps限帧。所有输入已 stop / allow1 off。

`[E-实测]` 启动前整体备份并核验41个 SaveGames/ELMP 文件；结束后停止 Host2/Client2，恢复本轮变化的2个 ELMP party JSON/备份文件，41项 SHA 全部一致，无新增存档文件需删除。记录 `build/client-camera-regression/saves-before/manifest.json`、`save-audit.json`。新 DLL 保留部署，实例停止。未操作正式游戏和正式存档。

本轮覆盖同图远近移动、Client 前视、目标污染清洗及 Host 本地共享镜头；未新增跨图 / Boss / 剧情镜头验收。
