// Chess clock LCD: reflective (polariser + reflector) seven-segment display, one window per
// half of the clock. Everything is drawn analytically with anti-aliased SDFs.
//
// instParams[0] = (side 0 ms, side 1 ms, flags, running side)   see src/scene/clock_model.h
//   flags: 1 unlimited (elapsed, --:-- when negative), 2 side 0 fallen, 4 side 1 fallen,
//          8 paused, 16 off, 32 dashes
// uv: window 0 uv.x in [0,1], window 1 uv.x in [2,3]; uv.y in [0,1] bottom to top.
// matParams[0] = (window aspect W/H, italic slant, reflector depth / window height, ghost strength)
// matParams[1] = (background albedo rgb, background roughness)
// matParams[2] = (segment albedo rgb, segment roughness)
// matParams[3] = (glass clear coat, glass roughness, segment shadow strength, min contrast at grazing view)

const int LCD_DIGITS[10] = int[10](63, 6, 91, 79, 102, 109, 125, 7, 127, 111);
const int LCD_MINUS = 64;

struct LcdState {
    int m[5];      // segment masks of the five digit positions  D0 : D1 D2 : D3 D4
    bool col1, col2, dp;
    bool run, pause, flag;
};

// Elongated hexagonal segment from a to b with thickness t (pointed 45 degree ends).
float lcdBar(vec2 p, vec2 a, vec2 b, float t) {
    vec2 d = b - a;
    float L = length(d);
    vec2 dir = d / L;
    vec2 q = p - 0.5 * (a + b);
    float al = abs(dot(q, dir));
    float pe = abs(dot(q, vec2(-dir.y, dir.x)));
    return max(pe - 0.5 * t, al + pe - 0.5 * L);
}

float lcdBox(vec2 p, vec2 c, vec2 h) {
    vec2 q = abs(p - c) - h;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);
}

float lcdTri(vec2 p, vec2 p0, vec2 p1, vec2 p2) {
    vec2 e0 = p1 - p0, e1 = p2 - p1, e2 = p0 - p2;
    vec2 v0 = p - p0, v1 = p - p1, v2 = p - p2;
    vec2 pq0 = v0 - e0 * clamp(dot(v0, e0) / dot(e0, e0), 0.0, 1.0);
    vec2 pq1 = v1 - e1 * clamp(dot(v1, e1) / dot(e1, e1), 0.0, 1.0);
    vec2 pq2 = v2 - e2 * clamp(dot(v2, e2) / dot(e2, e2), 0.0, 1.0);
    float s = sign(e0.x * e2.y - e0.y * e2.x);
    vec2 d = min(min(vec2(dot(pq0, pq0), s * (v0.x * e0.y - v0.y * e0.x)), vec2(dot(pq1, pq1), s * (v1.x * e1.y - v1.y * e1.x))),
                 vec2(dot(pq2, pq2), s * (v2.x * e2.y - v2.y * e2.x)));
    return -sqrt(d.x) * sign(d.y);
}

void lcdDigit(vec2 p, vec2 o, vec2 size, float t, int mask, inout float dLit, inout float dAll) {
    vec2 q = p - o;
    if (q.x < -0.15 || q.x > size.x + 0.15 || q.y < -0.15 || q.y > size.y + 0.15) return;
    float w = size.x, h = size.y, g = t * 0.3, hm = h * 0.5;
    float s[7];
    s[0] = lcdBar(q, vec2(g, h), vec2(w - g, h), t);
    s[1] = lcdBar(q, vec2(w, h - g), vec2(w, hm + g), t);
    s[2] = lcdBar(q, vec2(w, hm - g), vec2(w, g), t);
    s[3] = lcdBar(q, vec2(g, 0.0), vec2(w - g, 0.0), t);
    s[4] = lcdBar(q, vec2(0.0, g), vec2(0.0, hm - g), t);
    s[5] = lcdBar(q, vec2(0.0, hm + g), vec2(0.0, h - g), t);
    s[6] = lcdBar(q, vec2(g, hm), vec2(w - g, hm), t);
    for (int k = 0; k < 7; ++k) {
        dAll = min(dAll, s[k]);
        if ((mask & (1 << k)) != 0) dLit = min(dLit, s[k]);
    }
}

LcdState lcdState(float ms, uint flags, int side, int running, float t) {
    LcdState st;
    for (int k = 0; k < 5; ++k) st.m[k] = 0;
    st.col1 = false; st.col2 = false; st.dp = false;
    st.run = false; st.pause = false; st.flag = false;
    if ((flags & 16u) != 0u) return st;  // power off
    bool unlimited = (flags & 1u) != 0u;
    bool paused = (flags & 8u) != 0u;
    st.pause = paused;
    st.run = !paused && running == side;
    st.flag = (flags & (side == 0 ? 2u : 4u)) != 0u;
    bool colonOn = !paused || fract(t) < 0.5;
    if ((flags & 32u) != 0u || (unlimited && ms < 0.0)) {
        st.m[1] = LCD_MINUS; st.m[2] = LCD_MINUS; st.m[3] = LCD_MINUS; st.m[4] = LCD_MINUS;
        st.col2 = true;
        return st;
    }
    float v = max(ms, 0.0);
    if (!unlimited && v < 20000.0) {
        int tenths = int(floor(v / 100.0));
        int s = tenths / 10;
        st.m[2] = s >= 10 ? LCD_DIGITS[s / 10] : 0;
        st.m[3] = LCD_DIGITS[s % 10];
        st.m[4] = LCD_DIGITS[tenths % 10];
        st.dp = true;
        return st;
    }
    int secs = int(floor(v / 1000.0));
    int h = secs / 3600, mi = (secs / 60) % 60, s = secs % 60;
    if (h > 0) {
        st.m[0] = LCD_DIGITS[min(h, 9)];
        st.col1 = colonOn;
        st.m[1] = LCD_DIGITS[mi / 10];
    } else if (mi >= 10) {
        st.m[1] = LCD_DIGITS[mi / 10];
    }
    st.m[2] = LCD_DIGITS[mi % 10];
    st.col2 = colonOn;
    st.m[3] = LCD_DIGITS[s / 10];
    st.m[4] = LCD_DIGITS[s % 10];
    return st;
}

// Returns the distance to the lit segments; dAll = distance to every segment of the glass.
float lcdField(vec2 p, LcdState st, float aspect, float slant, out float dAll) {
    const float hD = 0.56, wD = 0.262, t = 0.064, pitch = 0.395, colGap = 0.15, y0 = 0.215;
    p.x -= (p.y - y0) * slant;
    float x4 = aspect - 0.25 - wD, x3 = x4 - pitch, x2 = x3 - pitch - colGap, x1 = x2 - pitch, x0 = x1 - pitch - colGap;
    float dLit = 1e3;
    dAll = 1e3;
    vec2 sz = vec2(wD, hD);
    lcdDigit(p, vec2(x0, y0), sz, t, st.m[0], dLit, dAll);
    lcdDigit(p, vec2(x1, y0), sz, t, st.m[1], dLit, dAll);
    lcdDigit(p, vec2(x2, y0), sz, t, st.m[2], dLit, dAll);
    lcdDigit(p, vec2(x3, y0), sz, t, st.m[3], dLit, dAll);
    lcdDigit(p, vec2(x4, y0), sz, t, st.m[4], dLit, dAll);
    // Colons and decimal point.
    vec2 dh = vec2(0.03);
    float c1x = 0.5 * (x0 + wD + x1) - 0.012, c2x = 0.5 * (x2 + wD + x3) - 0.012;
    float c1 = min(lcdBox(p, vec2(c1x, y0 + hD * 0.3), dh), lcdBox(p, vec2(c1x, y0 + hD * 0.7), dh));
    float c2 = min(lcdBox(p, vec2(c2x, y0 + hD * 0.3), dh), lcdBox(p, vec2(c2x, y0 + hD * 0.7), dh));
    float dp = lcdBox(p, vec2(0.5 * (x3 + wD + x4) - 0.01, y0 + 0.028), dh);
    dAll = min(dAll, min(min(c1, c2), dp));
    if (st.col1) dLit = min(dLit, c1);
    if (st.col2) dLit = min(dLit, c2);
    if (st.dp) dLit = min(dLit, dp);
    // Status icons in the left margin: running marker, pause bars, fallen flag.
    float run = lcdTri(p, vec2(0.17, y0 + hD * 0.78), vec2(0.37, y0 + hD * 0.78), vec2(0.27, y0 + hD * 1.0));
    float pau = min(lcdBox(p, vec2(0.22, y0 + hD * 0.89), vec2(0.03, 0.065)), lcdBox(p, vec2(0.32, y0 + hD * 0.89), vec2(0.03, 0.065)));
    float pole = lcdBox(p, vec2(0.16, y0 + hD * 0.3), vec2(0.018, hD * 0.3));
    float pennant = lcdTri(p, vec2(0.19, y0 + hD * 0.6), vec2(0.19, y0 + hD * 0.32), vec2(0.42, y0 + hD * 0.46));
    float flag = min(pole, pennant);
    dAll = min(dAll, min(min(run, pau), flag));
    if (st.run) dLit = min(dLit, run);
    if (st.pause) dLit = min(dLit, pau);
    if (st.flag) dLit = min(dLit, flag);
    return dLit;
}

void surface(in SurfaceInput i, inout Surface s) {
    float aspect = i.matParams[0].x, slant = i.matParams[0].y, depth = i.matParams[0].z, ghostK = i.matParams[0].w;
    int side = i.uv.x > 1.5 ? 1 : 0;
    vec2 luv = vec2(i.uv.x - 2.0 * float(side), i.uv.y);
    vec2 p = vec2(luv.x * aspect, luv.y);
    // Pixel footprint in window-height units (uniform control flow).
    vec2 fw = fwidth(p);
    float aa = max(max(fw.x, fw.y), 1e-4);

    uint flags = uint(max(i.instParams[0].z, 0.0) + 0.5);
    int running = int(floor(i.instParams[0].w + 0.5));
    float ms = side == 0 ? i.instParams[0].x : i.instParams[0].y;
    LcdState st = lcdState(ms, flags, side, running, i.time);

    // View and light directions in the window's tangent frame.
    vec3 T = i.tangentWS, B = i.bitangentWS, N = i.normalWS;
    vec3 V = i.viewDirWS;
    vec3 vt = vec3(dot(V, T), dot(V, B), dot(V, N));
    vec3 L = frame.sunDirection.xyz;
    vec3 lt = vec3(dot(L, T), dot(L, B), dot(L, N));
    if (lt.z < 0.25) lt = normalize(vec3(0.15, 0.7, 0.7));

    // Segments sit on the front glass; the reflector is 'depth' behind them: the eye sees the
    // reflector through a parallax offset and the segments' shadows fall on it.
    float dAll;
    float dLit = lcdField(p, st, aspect, slant, dAll);
    vec2 pr = p - vt.xy / max(vt.z, 0.15) * depth;
    vec2 ps = pr + lt.xy / max(lt.z, 0.25) * depth;
    float dAllS;
    float dShadow = lcdField(ps, st, aspect, slant, dAllS);
    float dSecond = lcdField(pr, st, aspect, slant, dAllS);

    float lit = clamp(0.5 - dLit / aa, 0.0, 1.0);
    float ghost = clamp(0.5 - dAll / aa, 0.0, 1.0);
    float shadow = clamp(0.5 - dShadow / (aa + 0.018), 0.0, 1.0);
    float reflectorDark = clamp(0.5 - dSecond / (aa + 0.01), 0.0, 1.0);

    float contrast = mix(i.matParams[3].w, 1.0, smoothstep(0.08, 0.55, vt.z));
    vec3 bg = i.matParams[1].rgb;
    // Faint cell texture of the reflector and a slight vignette towards the seal.
    float e = min(min(luv.x, 1.0 - luv.x) * aspect, min(luv.y, 1.0 - luv.y));
    float seal = 1.0 - smoothstep(0.015, 0.06, e);
    float topShade = smoothstep(0.75, 1.0, luv.y) * 0.12;
    vec3 base = bg * (1.0 - ghostK * ghost) * (1.0 - i.matParams[3].z * shadow * contrast) * (1.0 - 0.18 * reflectorDark * contrast);
    base *= (1.0 - topShade) * (1.0 - 0.75 * seal);
    vec3 segC = i.matParams[2].rgb;
    s.albedo = mix(base, segC, lit * contrast);
    s.roughness = mix(i.matParams[1].a, i.matParams[2].a, lit);
    s.metallic = 0.0;
    s.specular = mix(0.5, 0.3, lit);
    s.clearcoat = i.matParams[3].x;
    s.clearcoatRoughness = i.matParams[3].y;
    s.clearcoatNormalWS = i.normalWS;
    // A soft backlight (the light button of a digital clock): the reflector glows a little and the
    // dark segments mask it. Against a sunlit display (~20,000 nits reflected) it is nothing; in
    // the shade (the player of the black pieces sees the clock in the robot's shadow) it is what
    // keeps the digits readable.
    s.emission = base * (1.0 - lit * contrast) * 1500.0;
}
