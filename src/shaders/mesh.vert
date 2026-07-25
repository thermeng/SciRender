#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in float aScalar;

uniform mat4 uMVP;
uniform mat4 uModel; 
uniform float uPointSize; // ponytail: CPU-driven point size for point-cloud draw

out vec3 vNormal;
out vec3 vWorldPos;
out float vScalar;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    gl_PointSize = uPointSize; // ignored for triangle draws; set for GL_POINTS
    
    vWorldPos = vec3(uModel * vec4(aPos, 1.0)); 
    
    vNormal = aNormal;
    vScalar = aScalar;
}