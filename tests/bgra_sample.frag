#version 450
// 全屏三角形: 采样 W 图(带 components=(B,G,R,A) 映射的 sampled 视图), 输出到 OUT
layout(binding = 0) uniform sampler2D tex;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = texture(tex, vec2(0.5, 0.5));
}
