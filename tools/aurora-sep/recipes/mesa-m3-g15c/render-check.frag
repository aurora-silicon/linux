#version 450
// SPDX-License-Identifier: MIT
// vk-render-check: a colour that depends only on the pixel, so the host can
// check every pixel: (4x, 4y, 255 when x ^ y is odd, 255).
layout(location = 0) out vec4 color;
void main()
{
   ivec2 p = ivec2(gl_FragCoord.xy);
   color = vec4(float((p.x * 4) & 255) / 255.0, float((p.y * 4) & 255) / 255.0,
                float((p.x ^ p.y) & 1), 1.0);
}
