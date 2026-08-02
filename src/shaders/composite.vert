#version 460 core
out vec2 vUV;
void main() {
    float x = float(gl_VertexID & 1) * 4.0 - 1.0;
    float y = float((gl_VertexID >> 1) & 1) * 4.0 - 1.0;
    vUV = vec2(x, y) * 0.5 + 0.5;
    gl_Position = vec4(x, y, 1.0, 1.0);
}
