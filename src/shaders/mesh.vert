#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in float aScalar;

uniform mat4 uMVP;
uniform mat4 uModel; 
uniform mat4 uView;
uniform float uPointSize; // ponytail: CPU-driven point size for point-cloud draw

out vec3 vNormal;
out vec3 vWorldPos;
out vec3 vFragPos;
out float vScalar;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    gl_PointSize = uPointSize; // ignored for triangle draws; set for GL_POINTS
    
    // FIX: This calculation MUST be here for the fragment shader to read it!
    vWorldPos = vec3(uModel * vec4(aPos, 1.0)); 
    
    vFragPos = vec3(uView * uModel * vec4(aPos, 1.0));
    // World-space normal: lighting is done in world space so the lights stay
    // fixed in the world while the camera orbits. Model has no rotation/scale
    // here (identity), so the normal is used directly in world space.
    vNormal = aNormal;
    vScalar = aScalar;
}