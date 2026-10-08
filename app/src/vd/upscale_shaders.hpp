// VITA5 app — GLSL for the video upscale chain (see upscaler.hpp).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The EASU and RCAS shaders are ports of AMD's FidelityFX FSR 1.0
// (third_party/fsr/ffx_fsr1.h, MIT — third_party/licenses/FSR1-MIT.txt),
// FsrEasuF (line 315) and FsrRcasF (line 684). The 12-tap EASU gather is
// replaced by direct texelFetch of the same integer neighbours (the gathers
// were a fetch-count optimisation); the approximate rcp/rsq helpers are
// replaced with exact ones (more accurate, no cost on this GPU).
//
// Shader sources carry no #version line; the kit prepends it
// (hui::gfx::build_program).

#pragma once

namespace vd5::upscale_shaders
{

// One triangle covering the target; no vertex buffer (kit convention).
inline constexpr const char *kVertex = R"(
void main()
{
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// NV12 -> RGBA, matching nv12.cpp's BT.601 limited-range math exactly.
inline constexpr const char *kYuv = R"(
layout(location = 0) uniform sampler2D u_y;
layout(location = 1) uniform sampler2D u_uv;
out vec4 frag_color;

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    float yv = texelFetch(u_y, p, 0).r * 255.0 - 16.0;
    vec2 uvp = texelFetch(u_uv, ivec2(p.x / 2, p.y / 2), 0).rg * 255.0 - 128.0;
    float c = 298.0 * yv;
    vec3 rgb;
    rgb.r = clamp((c + 409.0 * uvp.y + 128.0) / 256.0, 0.0, 255.0);
    rgb.g = clamp((c - 100.0 * uvp.x - 208.0 * uvp.y + 128.0) / 256.0, 0.0, 255.0);
    rgb.b = clamp((c + 516.0 * uvp.x + 128.0) / 256.0, 0.0, 255.0);
    frag_color = vec4(rgb * (1.0 / 255.0), 1.0);
}
)";

// Plain bilinear stretch (Sharp mode's first pass).
inline constexpr const char *kBlit = R"(
layout(location = 0) uniform sampler2D u_src;
layout(location = 1) uniform vec2 u_out_size;
out vec4 frag_color;

void main()
{
    vec2 uv = gl_FragCoord.xy / u_out_size;
    frag_color = vec4(texture(u_src, uv).rgb, 1.0);
}
)";

// AMD FSR 1.0 EASU — ffx_fsr1.h FsrEasuF + FsrEasuTapF + FsrEasuSetF.
inline constexpr const char *kEasu = R"(
layout(location = 0) uniform sampler2D u_src;
layout(location = 1) uniform vec2 u_src_size;
layout(location = 2) uniform vec2 u_out_size;
out vec4 frag_color;

// Luma times 2 (ffx_fsr1.h: "Simplest multi-channel approximate luma").
float luma2(vec3 c)
{
    return c.b * 0.5 + (c.r * 0.5 + c.g);
}

vec3 fetch(ivec2 p)
{
    return texelFetch(u_src, clamp(p, ivec2(0), ivec2(u_src_size) - 1), 0).rgb;
}

// FsrEasuSetF: accumulate direction and length for one bilinear quadrant.
void easu_set(inout vec2 dir, inout float len, float w, float lA, float lB,
              float lC, float lD, float lE)
{
    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = 1.0 / max(lenX, 1e-6);
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
    lenX *= lenX;
    len += lenX * w;
    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = 1.0 / max(lenY, 1e-6);
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
    lenY *= lenY;
    len += lenY * w;
}

// FsrEasuTapF: windowed-lanczos2 tap.
void easu_tap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len,
              float lob, float clp, vec3 c)
{
    vec2 v;
    v.x = off.x * dir.x + off.y * dir.y;
    v.y = off.x * (-dir.y) + off.y * dir.x;
    v *= len;
    float d2 = v.x * v.x + v.y * v.y;
    d2 = min(d2, clp);
    float wB = (2.0 / 5.0) * d2 + (-1.0);
    float wA = lob * d2 + (-1.0);
    wB *= wB;
    wA *= wA;
    wB = (25.0 / 16.0) * wB + (-(25.0 / 16.0 - 1.0));
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

void main()
{
    ivec2 ip = ivec2(gl_FragCoord.xy);
    // Output integer position -> source texel-centre space (FsrEasuCon con0).
    vec2 scale = u_src_size / u_out_size;
    vec2 pfp = (vec2(ip) + 0.5) * scale - 0.5;
    vec2 pp = fract(pfp);

    // The 12-tap kernel around the 'f' tap.
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    ivec2 fp = ivec2(floor(pfp));
    vec3 b = fetch(fp + ivec2(0, -1));
    vec3 c = fetch(fp + ivec2(1, -1));
    vec3 i = fetch(fp + ivec2(-1, 1));
    vec3 j = fetch(fp + ivec2(0, 1));
    vec3 f = fetch(fp + ivec2(0, 0));
    vec3 e = fetch(fp + ivec2(-1, 0));
    vec3 k = fetch(fp + ivec2(1, 1));
    vec3 l = fetch(fp + ivec2(2, 1));
    vec3 h = fetch(fp + ivec2(2, 0));
    vec3 g = fetch(fp + ivec2(1, 0));
    vec3 o = fetch(fp + ivec2(1, 2));
    vec3 n = fetch(fp + ivec2(0, 2));

    float bL = luma2(b), cL = luma2(c), iL = luma2(i), jL = luma2(j);
    float fL = luma2(f), eL = luma2(e), kL = luma2(k), lL = luma2(l);
    float hL = luma2(h), gL = luma2(g), oL = luma2(o), nL = luma2(n);

    // Bilinear quadrant weights (s t / u v).
    vec2 dir = vec2(0.0);
    float len = 0.0;
    float wS = (1.0 - pp.x) * (1.0 - pp.y);
    float wT = pp.x * (1.0 - pp.y);
    float wU = (1.0 - pp.x) * pp.y;
    float wV = pp.x * pp.y;
    easu_set(dir, len, wS, bL, eL, fL, gL, jL);
    easu_set(dir, len, wT, cL, fL, gL, hL, kL);
    easu_set(dir, len, wU, fL, iL, jL, kL, nL);
    easu_set(dir, len, wV, gL, jL, kL, lL, oL);

    // Normalize and clean up close to zero.
    vec2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = inversesqrt(max(dirR, 1e-12));
    dirR = zro ? 1.0 : dirR;
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;

    // Shape the length, stretch the kernel along the gradient.
    len = len * 0.5;
    len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 + (-0.5) * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = 1.0 / max(lob, 1e-6);

    // Dering: clip against the 4 nearest taps.
    vec3 min4 = min(min(f, g), min(j, k));
    vec3 max4 = max(max(f, g), max(j, k));

    vec3 aC = vec3(0.0);
    float aW = 0.0;
    easu_tap(aC, aW, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, b);
    easu_tap(aC, aW, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, c);
    easu_tap(aC, aW, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, i);
    easu_tap(aC, aW, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
    easu_tap(aC, aW, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, f);
    easu_tap(aC, aW, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);
    easu_tap(aC, aW, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, k);
    easu_tap(aC, aW, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, l);
    easu_tap(aC, aW, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, h);
    easu_tap(aC, aW, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
    easu_tap(aC, aW, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, o);
    easu_tap(aC, aW, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, n);

    frag_color = vec4(min(max4, max(min4, aC * (1.0 / max(aW, 1e-6)))), 1.0);
}
)";

// AMD FSR 1.0 RCAS — ffx_fsr1.h FsrRcasF (5-tap cross, per-channel limiter).
// u_sharp is FsrRcasCon's con[0] = 2^(-sharpness stops); 1.0 = full sharpness.
inline constexpr const char *kRcas = R"(
layout(location = 0) uniform sampler2D u_src;
layout(location = 1) uniform float u_sharp;
out vec4 frag_color;

const float kRcasLimit = 0.25 - (1.0 / 16.0); // FSR_RCAS_LIMIT

vec3 load(ivec2 p, ivec2 s)
{
    return texelFetch(u_src, clamp(p, ivec2(0), s - 1), 0).rgb;
}

float luma2(vec3 c)
{
    return c.b * 0.5 + (c.r * 0.5 + c.g);
}

void main()
{
    ivec2 sp = ivec2(gl_FragCoord.xy);
    ivec2 s = textureSize(u_src, 0);
    //    b
    //  d e f
    //    h
    vec3 b = load(sp + ivec2(0, -1), s);
    vec3 d = load(sp + ivec2(-1, 0), s);
    vec3 e = load(sp, s);
    vec3 f = load(sp + ivec2(1, 0), s);
    vec3 h = load(sp + ivec2(0, 1), s);

    // Min and max of the ring, per channel.
    vec3 mn4 = min(min(b, d), min(f, h));
    vec3 mx4 = max(max(b, d), max(f, h));

    // Limiters ("these need to be high precision RCPs" — exact here).
    vec3 hitMin = min(mn4, e) * (1.0 / max(4.0 * mx4, vec3(1e-6)));
    vec3 den = max(4.0 * mn4 - 4.0, vec3(-1e-6));
    vec3 hitMax = (1.0 - max(mx4, e)) / den;
    vec3 lobe = max(-hitMin, hitMax);
    // Pick the strongest channel's lobe, clamp it, apply the sharpness scale.
    float lobeMax = max(lobe.r, max(lobe.g, lobe.b));
    float w = max(-kRcasLimit, min(lobeMax, 0.0)) * u_sharp;
    // Resolve: output = (w*(b+d+f+h)+e)/(4*w+1).
    float rcpL = 1.0 / (4.0 * w + 1.0);
    frag_color = vec4((w * (b + d + f + h) + e) * rcpL, 1.0);
}
)";

} // namespace vd5::upscale_shaders
