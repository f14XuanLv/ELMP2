# 协议 22 四端离会与原版潜水命令

ELMP v2，2026-11-10，现场构建 `4b8c080`。四端 Dev DLL SHA-256 均为 `6ee56f8dad3567f4ecc074f4cb0cc3761dce97dce97c3e3685faefc3d577c24d`。原始日志和只读查询保存在 `build/p22-party-exit-20261110-234440/`。

- [E-实测] 三客人跟随 Host 进入 map_forest_03。23:43:38.920431，Host 记录 `coop: leaving (native Lily frame encoding)`。Client1、Client3、Client2 分别在约 5、9、18 ms 后收到 `peer closed: left the session`，随后回到标题。归档时四进程均存活；Host 仍在该图，网络角色为 none。
- [源码对照] Host 在 flush_native 逐对端编码待发 LilyFrame；encode 返回 0 就设置全局 lifecycle_fault，Host 的 tick 随后结束会话。该消息没有帧字段、座位、命令或接收者，无法由日志反查具体失败条件。应用层 leave 只写本地日志；Endpoint 销毁时向三个对端发送相同的默认关闭文本。客户端确认收到 Host 主动关闭，显示的文字没有区分手动关房和内部故障。
- [E-实测] 退房后只读枚举 Host 的原生命令对象，找到了 P1 自己的 CommandQueueComponent.CA_Dive_C，方向为 2、调用输入为 100。当前游戏枚举将 100 定义为 ANY。[E-CUE] DATA_Command_Dive 的 CommandSettings 明确配置 ANY、向下三个方向、Dive 能力与 MOVE_Swimming，动作类为 CA_Dive_C。
- [源码对照] 旧 LilyFrame 编码器对 Start/End 及持续命令均检查 button ≤ 9。Host 捕获的是原版动作的 GetInvokedByInputType；若采到上述动作，其合法值必然被拒。此为本次最直接的字段级解释。失败帧已在拆房时清除，Host 两份详细轨迹均为空，仍无法确认首次被拒帧的座位、类或字段。
- 修正原版调用输入枚举范围，同时保留上行玩家按键掩码。莉莉动作、持续动作及自主精灵共享枚举校验；失败的莉莉帧记录字段、值、索引、座位、序号、地图、视图及接收对端，命令项另记类和 token。主动离会保留本地原因并传入原有的限长关闭帧。此次调查保留四端现场，未部署或重启游戏。
