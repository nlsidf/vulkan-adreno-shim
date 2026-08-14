#version 450
layout(push_constant) uniform PC {
    vec4 color;
    float z;
} pc;
void main() {
    vec2 p[3] = vec2[3](vec2(-0.6,-0.6), vec2(0.6,-0.6), vec2(0.0,0.7));
    gl_Position = vec4(p[gl_VertexIndex], pc.z, 1.0);
}
