# ADR0019：Client权限维护与本地交互边界（2026-11-08）

状态：已实施，真实双端换图、原生入口拒绝与Host恢复客人状态验证通过。安全坐姿/受限菜单为独立规划，不在本修复中直接开放EVT_Rest。

[E-实测] Client仍联机时本机P1播放rest_loop、StateEvent/StateInvincible存在、WBP_RestMenu在视口；Host两只莉莉均idle。Client保存拦截计数3，说明后置保存保护存在，但完整休息流程已在本地执行。用户回忆独立过图往返后发生，此顺序不能仅凭回忆认定，但与维护缺口相符。

[E-代码] permission::tick唯一调用位于party::respawn::patrol，而该phase只在party::count()>0时启用。Client副本是core角色，不是本地party座位，因此Client通常count=0：Welcome时p1_presentation(true)只应用一次，后续新地图目标/新Pawn无法持续刷新。另有LoadMap清空记录而保留会话模式，相同p1_presentation设置直接返回；hold_p1_doors的managed计算遗漏Client模式。逻辑allows(1)也把物理Controller0等同于权限主体P1，忽略它在Client是客人的事实。

决定：权限维护归core独立phase，有受管对象或P1模式时执行，按Pawn/注册表版本/开关失效，不依赖party巡检。Client模式/规则2保持所需P1记录，换世界后重建；普通无座位无会话保持G0。Client物理P1的本地世界权限关闭；Host P1语义不变。

碰撞忽略只负责空间接触，不能替代按键时的交互许可。Client模式在原生InteractablePressInteract（已存在manifest入口）阻止本地世界交互副作用，避免已有InteractingController或加载早期接触穿过旧碰撞缓存。网络输入继续送Host，未来安全坐姿/菜单走Host授权的独立呈现入口。启用此模式必须成功安装该边界，否则不入座；增加模式、维护次数、重建/阻止计数与当前目标审计。

参考LilyInEM仅用于设计与事实：g_body.cpp独立Create/AddToViewport休息菜单；隐藏Save/Leave/Recollection/Challenge等、拦截对应处理函数、重接导航。ELMP不复制其代码或其菜单授权集合，特别不默认开放强化/旅行。业务方向记录于REQUIREMENTS-TREE F1.9。

验证用Dev `elmp gates probe-press`直接调用已校验的原生交互按下入口，只允许在Client边界已武装且有真实椅子对象时运行；核原生detour拒绝计数并检查菜单/事件/存档。后台键盘消息可能未到达原生入口，不能把计数未变化误当边界已经被动态验证。此辅助不进入Shipping、不激活窗口、不投递鼠标消息。

[E-实测] 独立切图往返后，Client P1在church_07忽略4/4个目标，回church_08忽略7/7；强制ActivateInteractable保留旧交互引用后，调用原生入口使localPressBlocked从0到1，没有StateEvent/休息菜单。Client INTERACT仍使Host P2从200恢复到400，Host P1不受影响；Client原版存档哈希不变。详见[kb](../kb/bench-and-derived-actions-2026-11-08.md)。
