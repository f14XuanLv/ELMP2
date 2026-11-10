# 同图四人实测（ELMP v2，2026-11-10）

基线 7076aaa，工作分支 codex/remote-party-replay，协议 21。已复用 f7ffabb 经实证的 Spine 重入与弹体结束顺序，工具使用改名后的四个隔离实例和游戏线程 UEBridge。

- [E-实测] church_07，P1 留在 Y=8530；将 P2/P3/P4 分别放在 Y=3700/1540/−2900。分散前 e2010_Slime18、e2020_Soldier3、e2021_Soldier3 在 Host 均 Paused=true、Actor Tick 关。分散后三只均 Paused=false、Tick 开、取得目标并移动。Client1/2/3 的同名出生点读到同方向位置变化，AI 仍由 Host 执行。该结论覆盖三只地图原生敌人。关卡激活体积与 Boss 另有入口。证据 build/party-1110/{church-initial,spread-before,spread-0..4}.json。
- [E-实测] 只在 Host 接受原生命令，Client 通过原版 AddNewImmediateCommand 执行队列外层动作，命令内派生仍归原版。第一版错误地另发组合子动作，三个 Client 的 Start 记录均出现重复；以原版队首 UCommand.CommandAction 的身份收紧后，在相同来源的连段恢复一份启动。保留连续的原版输入 / Tick；冷加入只恢复可安全进入的持续动作。双端动作采样保存于 build/party-1110/edges-*.json。
- [E-实测] 四端同时在场，Host P1 在 church_07 从 350、1300、200、500 UU 下砸，实际依次选中二、三、一、二档。每轮 Host 生成 3 只 1 HP 敌人，攻击前检查 Client1 副本，逐帧记录全部 12 只 HP 归零；四端逐次比较能力类、输入来源、子路径均一致。Client1 各轮绑定增量为 2。见 build/party-1110/{slam.log,repro.json,slam-case-*}。
- [E-实测] 仅在 Client2 关闭远端 P1 呈现副本的 Actor Tick，触及原 120 帧上限（5253 / 5133）。该 Client 的视图从 2 更新为 3，原网络座位 P3 保留，从新基线恢复消费；Host 保持三个 peer，Client1/3 的视图仍为 2、刷新计数 0。随后同一会话完成上述四轮下砸。刷新单位为一个 Client 的呈现视图，包括同图世界副本，所有客人结算闸门保持开启。证据 build/party-1110/gap.log 与 observe1/。未重播旧视图中已过期的一次性表现。
- [E-实测] 首轮 720p 四窗口启动，主屏物理分辨率 2560×1600，工作区 2560×1528；GetClientRect / ClientToScreen 实读四端客户区均 1280×720，原点依次 (0,0)、(1280,0)、(0,808)、(1280,808)。
- 第一轮低台夹具只触发 CharacterMovement 普通跨步，Host 轨迹没有 CA_LedgeClimbLow.Start。这一轮不能计为攀台验收。四端当前覆盖的技能、地图和分支均以上述记录为限。
