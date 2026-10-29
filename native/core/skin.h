#pragma once

// Skins (F1.12, FR-CORE-12): the six vanilla Lily looks p0001..p0006 (DT_SpineData_p0000 has 11 skin
// levels but six looks: atlas + skeleton p000N, notify always p0001, light MI_LilyLightN). The vanilla
// FindSpineDataForLevel reads P1's skin level, so a PX's look is applied directly to its Spine
// component — the same steps as filling in Spine data after spawning (BP_p0000_Lily has none by
// default, v1 core-facts「生成第 N 玩家」). P1 never goes through here.

#include "engine/ue.h"

#include <string>

namespace elmp::core::skin {

inline constexpr int kDefault = 1, kCount = 6;

constexpr bool valid(int n) { return n >= 1 && n <= kCount; }

std::string apply(ue::UObject* pawn, int n); // empty = ok
int current(ue::UObject* pawn);              // 0 = unknown

// 自建呈现用 Pawn 的“蓝图默认值”会丢：实测（2026-10-30，Client2 church_07）复制出来的 Host 莉莉
//   · SpineSkeletonRenderer.RelativeLocation.Z 从 -65 变成 0 → 整只凭空高半身；
//   · ColorComponent 的当前颜色从白 (1,1,1,1) 变成 (0,0,0,1) → 只剩黑色剪影。
// 两者都不是运行时状态，而是 BP 组件模板上的默认值，所以从同类的 CDO 抄回来即可（不写死数值）。
// 幂等：值本来就对时什么也不改。
void normalize(ue::UObject* pawn);

// “踩地”偏移：原版 USpineAnimationComponent 会在骨骼更新后把 SpineSkeletonRenderer 往下挪（莉莉 -65、e2021 -50），
// 客户端不跑那条 tick，自建副本/被冻结的敌人会停在 0 → 高一个身位。Host 用 renderer_z 采样，随快照下发；
// 客户端用 set_renderer_z 应用。两者都不写死数值。
float renderer_z(ue::UObject* actor);                     // 0 表示读不到
bool set_renderer_z(ue::UObject* actor, float z);         // 变化超过 0.5 才写
bool set_renderer_z_comp(ue::UObject* renderer, float z); // 已缓存渲染组件时用这个（每帧路径）

} // namespace elmp::core::skin
