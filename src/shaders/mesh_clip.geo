#version 460 core
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;

layout(std140) uniform MeshUBO {
    mat4  uMVP;
    mat4  uModel;
    vec4  uViewPos_PS;
    vec4  uMeshColor_Wire;
    vec4  uSurfaceColor_Op;
    vec4  uPointClip;
    vec4  uLightDir;
    vec4  uLightFill;
    vec4  uLightBack1;
    vec4  uLightBack2;
    vec4  uLightHead;
    vec4  uKeyColor;
    vec4  uFillColor;
    vec4  uBackColor;
    vec4  uHeadColor;
    vec4  uScalars;
    vec4  uSliceY;
    vec4  uSliceEn;
    vec4  uInvert;
    vec4  uFilter;
    vec4  uMaterial;
    vec4  uIntensities;
    vec4  uPBR;
    vec4  uShadingMode;
};

in MeshVarying {
    vec3 vWorldPos;
    vec3 vNormal;
    float vScalar;
} gs_in[];

out MeshVarying {
    vec3 vWorldPos;
    vec3 vNormal;
    float vScalar;
} gs_out;

void main() {
    if (uPointClip.w > 0.5) {
        bool cull = false;
        if (bool(uSliceEn.x)) {
            bool allBehind = true;
            for (int i = 0; i < 3; ++i) {
                bool behind = (uInvert.x > 0.5)
                    ? (gs_in[i].vWorldPos.x < uSliceY.x)
                    : (gs_in[i].vWorldPos.x > uSliceY.x);
                if (!behind) { allBehind = false; break; }
            }
            if (allBehind) cull = true;
        }
        if (!cull && bool(uSliceEn.y)) {
            bool allBehind = true;
            for (int i = 0; i < 3; ++i) {
                bool behind = (uInvert.y > 0.5)
                    ? (gs_in[i].vWorldPos.y < uSliceY.y)
                    : (gs_in[i].vWorldPos.y > uSliceY.y);
                if (!behind) { allBehind = false; break; }
            }
            if (allBehind) cull = true;
        }
        if (!cull && bool(uSliceEn.z)) {
            bool allBehind = true;
            for (int i = 0; i < 3; ++i) {
                bool behind = (uInvert.z > 0.5)
                    ? (gs_in[i].vWorldPos.z < uSliceY.z)
                    : (gs_in[i].vWorldPos.z > uSliceY.z);
                if (!behind) { allBehind = false; break; }
            }
            if (allBehind) cull = true;
        }
        if (cull) return;
    }

    for (int i = 0; i < 3; ++i) {
        gl_Position = gl_in[i].gl_Position;
        gs_out.vWorldPos = gs_in[i].vWorldPos;
        gs_out.vNormal = gs_in[i].vNormal;
        gs_out.vScalar = gs_in[i].vScalar;
        EmitVertex();
    }
    EndPrimitive();
}
