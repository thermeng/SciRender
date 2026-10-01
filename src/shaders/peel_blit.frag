#version 460 core

// Fullscreen helper for dual depth peeling. Three jobs, selected by uMode,
// sharing one program so there is a single fullscreen-triangle draw helper.
//
//   0 SEED_DEPTH - initialise both depth-range ping-pong buffers from the opaque
//                  scene depth. Pixels with no opaque geometry are discarded,
//                  leaving the (-1,-1) sentinel that means "nothing translucent
//                  resolved yet"; the opaque depth becomes the far bound so
//                  nothing behind it can ever contribute.
//
//   1 ACCUM_BACK - blend one iteration's back peel into the back accumulator.
//                  over-blend on premultiplied colour:
//                    dst.rgb += src.rgb * (1 - dst.a)
//                    dst.a   += src.a   * (1 - dst.a)
//                  done by fixed function with GL_ONE / GL_ONE_MINUS_SRC_ALPHA,
//                  so this shader only has to forward src.
//
//   2 FINAL      - resolve. Peeling is done, so under-blend the back buffer
//                  BENEATH the front accumulator to form the translucent layer
//                  t, then let the hardware blend t OVER the opaque scene:
//
//                    front.a = 1 - front.a        // stored as (1 - alpha)
//                    t.rgb   = front.rgb + back.rgb * front.a
//                    t.a     = 1 - front.a * (1 - back.a)   // under -> over
//
//                  The final `1 -` converts the under-blend alpha into an
//                  over-blend alpha, because GL's blend equation needs the
//                  latter. Note the front accumulator is only ever written by
//                  the front peel -- skipping it silently drops half the
//                  resolved layers and the surface reads far too opaque.
//
// Unlike the old composite.frag this needs exactly two samplers regardless of
// peel count, so there is no GL_MAX_TEXTURE_IMAGE_UNITS ceiling and no
// separate iterative fallback path.

in vec2 vUV;
uniform sampler2D uSrc;    // SEED: opaque depth. ACCUM_BACK: backTemp.
uniform sampler2D uBack;   // FINAL: back accumulator.
uniform int uMode = 1;
uniform float uClearValue = 1.0;

layout(location = 0) out vec4 FragColor0;
layout(location = 1) out vec4 FragColor1;

void main() {
    ivec2 pix = ivec2(gl_FragCoord.xy);
    vec4 s = texelFetch(uSrc, pix, 0);

    if (uMode == 0) {
        // uSrc is the opaque depth texture.
        if (s.r >= uClearValue) discard;   // no opaque geometry at this pixel
        // near = -1 sentinel (nothing translucent resolved yet), far = opaque.
        FragColor0 = vec4(-1.0, s.r, 0.0, 0.0);
        FragColor1 = vec4(-1.0, s.r, 0.0, 0.0);
        return;
    }

    if (uMode == 1) {
        // ACCUM_BACK: forward premultiplied src; fixed function does the
        // GL_ONE / GL_ONE_MINUS_SRC_ALPHA over-blend.
        FragColor0 = s;
        return;
    }

    // FINAL: under-blend back beneath front, then emit an over-blend alpha.
    vec4 back = texelFetch(uBack, pix, 0);
    float fA = 1.0 - s.a;                 // s is the front accumulator
    FragColor0 = vec4(s.rgb + back.rgb * fA,
                      1.0 - fA * (1.0 - back.a));
}