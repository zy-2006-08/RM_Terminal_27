.pragma library

// 结算动画的缓动与取值助手。动画由一个显式的毫秒时钟 tMs 驱动(见 VictoryOverlay.qml)
// 而不是 NumberAnimation,所以缓动曲线在这里自己算。
//
// 用显式时钟的理由:验收要求在 t≈0.3s / 1.0s / 2.0s / 3.5s 各截一张图,而
// NumberAnimation 按墙上时间推进 —— grab() 抓到哪一帧取决于调度抖动,同一条命令
// 两次跑出来的图不一样,既没法逐像素比对,也没法在测试里断言某个时刻的状态。

// 把 t 映射到 [0,1]:start 之前是 0,start+dur 之后是 1。
function span(t, start, dur) {
    if (dur <= 0) return t >= start ? 1 : 0
    return Math.max(0, Math.min(1, (t - start) / dur))
}

function outCubic(p) { var q = 1 - p; return 1 - q * q * q }
function outQuad(p) { var q = 1 - p; return 1 - q * q }
function outQuint(p) { var q = 1 - p; return 1 - q * q * q * q * q }
function inOutSine(p) { return 0.5 - 0.5 * Math.cos(Math.PI * p) }

function mix(a, b, p) { return a + (b - a) * p }

// 固定种子的伪随机序列(mulberry32)。爆发层的条纹形状靠它生成,不能用
// Math.random():那样每次重画(窗口缩放、重新显示)条纹都会跳到别的位置,
// 而同一次结算里这团光必须是同一团。换一种结果时传不同种子,红/蓝/平局才不同形。
function seeded(seed) {
    var state = seed >>> 0
    return function () {
        state = (state + 0x6D2B79F5) >>> 0
        var z = state
        z = Math.imul(z ^ (z >>> 15), z | 1) >>> 0
        z = (z ^ (z + Math.imul(z ^ (z >>> 7), z | 61))) >>> 0
        return ((z ^ (z >>> 14)) >>> 0) / 4294967296
    }
}

// span + 缓动 + 插值,动画里最常见的那个组合。
function ramp(t, start, dur, from, to, ease) {
    var p = span(t, start, dur)
    return mix(from, to, ease ? ease(p) : p)
}

// 徽章弹入:0.5 → 1.08 → 1.0。两段而不是 Easing.OutBack,因为过冲峰值要正好落在
// 1.08(验收要求),OutBack 的峰值由 overshoot 系数间接决定,调不准。
function popScale(t, start, rise, settle) {
    if (t <= start) return 0.5
    if (t < start + rise) return mix(0.5, 1.08, outQuad((t - start) / rise))
    return mix(1.08, 1.0, inOutSine(span(t, start + rise, settle)))
}
