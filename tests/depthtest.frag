#version 450
layout(push_constant) uniform PC {
    vec4 color;
    float z;
} pc;
layout(location=0) out vec4 outColor;
void main() {
    outColor = pc.color;
}
