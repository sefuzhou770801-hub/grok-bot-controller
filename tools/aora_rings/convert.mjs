// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// aora-bot (github.com/sam70361/aora-bot) emotion-ball 眼环数据转换器。
// 读上游 rings.js / emotions.js（路径经 AORA_PATH 提供，上游源码不入仓），
// 把我们 15 个表情映射到的状态的眼环轮廓平移缩放到本机屏幕坐标系
// （球心 160,120，半径 100），连同轮换/开合/动画参数生成
// main/aora_ring_data.hpp。上游更新后重跑本脚本再编译即可同步。
//
// 用法: AORA_PATH=/path/to/aora-bot node tools/aora_rings/convert.mjs
import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { execSync } from 'node:child_process';

const AORA = process.env.AORA_PATH;
if (!AORA) throw new Error('set AORA_PATH to the aora-bot checkout');
const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '../..');

function loadWindowScript(path) {
  const window = {};
  new Function('window', readFileSync(path, 'utf8'))(window);
  return window;
}

const RINGS = loadWindowScript(`${AORA}/emotion-ball/js/rings.js`).EB_RINGS;
const SEED = loadWindowScript(`${AORA}/emotion-ball/js/emotions.js`).EMOTION_SEED;
const byId = new Map(SEED.map((e) => [e.id, e]));
let upstream = 'unknown';
try {
  upstream = execSync(`git -C ${AORA} rev-parse --short HEAD`).toString().trim();
} catch {}

// 我们的表情枚举 → aora 状态 id
const MAP = [
  ['Neutral', '02'], ['Happy', '10'], ['Sad', '12'], ['Angry', '21'],
  ['Doubt', '11'], ['Sleepy', '00'], ['Listening', '35'], ['Thinking', '30'],
  ['Excited', '33'], ['Curious', '03'], ['Confused', '20'], ['Surprised', '13'],
  ['Dizzy', '17'], ['Affection', '14'], ['Bored', '04'],
];

// 缩放：aora 头心 HEAD_C，把所有用到的环装进我们的球（r=100，留边）。
const HEAD = RINGS.HEAD_C;
const usedRings = [...new Set(MAP.flatMap(([, id]) => byId.get(id).pool))].sort((a, b) => a - b);
let maxD = 0;
for (const ri of usedRings) {
  for (const ring of RINGS.EXPRESSIONS[ri]) {
    for (const [x, y] of ring) {
      maxD = Math.max(maxD, Math.hypot(x - HEAD, y - HEAD));
    }
  }
}
const SCALE = 88 / maxD; // 环最远点落在半径 88，眼睛不顶球边
console.log(`used rings: ${usedRings.join(',')}  maxD=${maxD.toFixed(1)}  scale=${SCALE.toFixed(3)}`);

const ringIndex = new Map(usedRings.map((r, i) => [r, i]));
// C++ f32 字面量：必须带小数点（77f 非法，77.0f 合法）。
const f = (v) => {
  let s = v.toFixed(2).replace(/(\.\d*?)0+$/, '$1').replace(/\.$/, '');
  if (s === '-0') s = '0';
  if (!s.includes('.')) s += '.0';
  return s;
};

// 环数据：屏幕坐标（球心 160,120）
const ringRows = usedRings.map((ri) => {
  const sides = RINGS.EXPRESSIONS[ri].map((ring) =>
    ring.map(([x, y]) => `${f(160 + (x - HEAD) * SCALE)}f, ${f(120 + (y - HEAD) * SCALE)}f`).join(', ')
  );
  return `    { // aora ring ${ri}\n        {${sides[0]}},\n        {${sides[1]}},\n    },`;
});

// 表情配置
const ANIM_KIND = { sine: 0, glance: 1, jitter: 2, scan: 3 };
const TARGET = { eyes: 0, left: 1, right: 2 };
const cfgRows = MAP.map(([name, id]) => {
  const e = byId.get(id);
  const pool = e.pool.map((r) => ringIndex.get(r));
  while (pool.length < 6) pool.push(pool[0]);
  const poolMs = e.poolMs ? (e.poolMs[0] + e.poolMs[1]) / 2 : 6000;
  const openness = e.openness ?? 1;
  const eyes = e.eyes ?? {};
  const off = (side, axis) => {
    const both = eyes.both ?? {};
    const own = eyes[side] ?? {};
    const key = axis === 0 ? ['x', 'lookX'] : ['y', 'lookY'];
    return (both[key[0]] ?? 0) + (both[key[1]] ?? 0) + (own[key[0]] ?? 0) + (own[key[1]] ?? 0);
  };
  const anims = (e.anims ?? [])
    .filter((a) => (a.target === 'eyes' || a.target === 'left' || a.target === 'right') &&
                   (a.prop === 'lookX' || a.prop === 'lookY' || a.prop === 'x' || a.prop === 'y') &&
                   ANIM_KIND[a.type] !== undefined)
    .slice(0, 3)
    .map((a) => {
      const axis = a.prop === 'lookX' || a.prop === 'x' ? 0 : 1;
      const period = a.period ?? (a.speed ? 1000 / a.speed : 1000);
      const phase = a.phaseMs ?? (a.phase ? (a.phase / (2 * Math.PI)) * period : 0);
      return `{${ANIM_KIND[a.type]}, ${axis}, ${TARGET[a.target]}, ${f(a.amp * SCALE)}f, ${f(period)}f, ${f(phase)}f}`;
    });
  while (anims.length < 3) anims.push('{0, 0, 0, 0.0f, 1.0f, 0.0f}');
  return `    { /* ${name} <- aora ${id} ${e.name} */\n` +
         `        {${pool.join(', ')}}, ${e.pool.length}, ${f(poolMs)}f, ${f(openness)}f,\n` +
         `        ${f(off('left', 0) * SCALE)}f, ${f(off('left', 1) * SCALE)}f, ` +
         `${f(off('right', 0) * SCALE)}f, ${f(off('right', 1) * SCALE)}f,\n` +
         `        {${anims.join(',\n         ')}}, ${(e.anims ?? []).filter((a) => TARGET[a.target] !== undefined && ANIM_KIND[a.type] !== undefined && ['lookX','lookY','x','y'].includes(a.prop)).slice(0,3).length},\n    },`;
});

const hpp = `// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// 本文件由 tools/aora_rings/convert.mjs 生成，勿手改。
// 数据来源：aora-bot (github.com/sam70361/aora-bot) emotion-ball
// rings.js / emotions.js，上游 commit ${upstream}。眼环轮廓与行为参数
// 按其社区许可用于非商业用途，形象（身体造型/配色）未移植。
#pragma once

#include <cstdint>

namespace stackchan::app::aora {

inline constexpr std::size_t kRingPoints = 48;
inline constexpr std::size_t kRingCount = ${usedRings.length};

// 每环：左右眼各 48 点屏幕坐标 (x0,y0,x1,y1,...)，球心 160,120。
inline constexpr float kRings[kRingCount][2][kRingPoints * 2] = {
${ringRows.join('\n')}
};

// anims: kind 0=sine 1=glance 2=jitter 3=scan; axis 0=x 1=y;
// target 0=both 1=left 2=right。amp 已按环缩放折算成屏幕像素。
struct AnimCfg {
    std::uint8_t kind;
    std::uint8_t axis;
    std::uint8_t target;
    float amp;
    float period_ms;
    float phase_ms;
};

struct ExprCfg {
    std::uint8_t pool[6];
    std::uint8_t pool_n;
    float pool_ms;
    float openness;
    float left_dx, left_dy, right_dx, right_dy;
    AnimCfg anims[3];
    std::uint8_t anim_n;
};

// 下标 = stackchan::avatar::Expression 枚举值（0..14）。
inline constexpr ExprCfg kExprCfg[15] = {
${cfgRows.join('\n')}
};

} // namespace stackchan::app::aora
`;

writeFileSync(resolve(ROOT, 'main/aora_ring_data.hpp'), hpp);
console.log(`main/aora_ring_data.hpp written: ${usedRings.length} rings, 15 expr cfgs`);
