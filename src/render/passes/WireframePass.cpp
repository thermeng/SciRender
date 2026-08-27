#include "render/passes/WireframePass.h"
#include "render/foundation/renderer.h"
#include "render/foundation/shader_utils.h"
#include <glm/gtc/type_ptr.hpp>

void WireframePass::init(const ShaderSources& sources) {
    if (!sources.meshWireVert.empty() && !sources.meshWireGeo.empty() && !sources.meshWireFrag.empty()) {
        wireProgram.reset(compileProgramWithGS(sources.meshWireVert.c_str(), sources.meshWireGeo.c_str(), sources.meshWireFrag.c_str(), "MeshWire"));
        if (wireProgram.has()) {
            wireMvpLoc = glGetUniformLocation(wireProgram, "uMVP");
            wireViewportLoc = glGetUniformLocation(wireProgram, "uViewport");
            wireHalfWidthLoc = glGetUniformLocation(wireProgram, "uHalfWidth");
            wireColorLoc = glGetUniformLocation(wireProgram, "uWireColor");
            wireModelLoc = glGetUniformLocation(wireProgram, "uModel");
            wireSliceYLoc = glGetUniformLocation(wireProgram, "uSliceY");
            wireSliceEnLoc = glGetUniformLocation(wireProgram, "uSliceEn");
            wireInvertLoc = glGetUniformLocation(wireProgram, "uInvert");
            wireClipEnabledLoc = glGetUniformLocation(wireProgram, "uClipEnabled");
        }
    }
}
void WireframePass::shutdown() {
    wireProgram.reset();
    wireViewportLoc = wireHalfWidthLoc = wireMvpLoc = wireColorLoc = -1;
    wireModelLoc = wireSliceYLoc = wireSliceEnLoc = wireInvertLoc = wireClipEnabledLoc = -1;
}

void WireframePass::draw(const RenderRenderState& state, MeshUBOData& ubo, GLuint meshUbo,
                         const std::vector<std::pair<GLuint,int>>& drawList,
                         const std::vector<std::pair<GLuint,int>>& edgeDrawList) {
    const bool useCrinkleClip = state.crinkleClipMode;
    const bool hasEdges = !edgeDrawList.empty();
    const bool useCellEdges = state.showWireframe && hasEdges;

    if (state.showWireframe && useCellEdges) {
        GLStateGuard guard;
        ubo.meshColor_wire = glm::vec4(state.meshColor[0], state.meshColor[1], state.meshColor[2], 1.0f);
        glNamedBufferSubData(meshUbo, offsetof(MeshUBOData, meshColor_wire), sizeof(glm::vec4), &ubo.meshColor_wire);
        const bool useThickWire = wireProgram.has()
                                  && wireMvpLoc >=0 && wireColorLoc>=0 && wireViewportLoc>=0 && wireHalfWidthLoc>=0;
        if (useThickWire) {
            GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
            glUseProgram(wireProgram);
            glUniformMatrix4fv(wireMvpLoc,1,GL_FALSE, glm::value_ptr(ubo.mvp));
            glUniform4fv(wireColorLoc,1, glm::value_ptr(ubo.meshColor_wire));
            glUniform2f(wireViewportLoc, float(vp[2]), float(vp[3]));
            glUniform1f(wireHalfWidthLoc, 0.5f * state.lineWidth);
            glUniformMatrix4fv(wireModelLoc, 1, GL_FALSE, glm::value_ptr(ubo.model));
            glUniform4fv(wireSliceYLoc, 1, glm::value_ptr(ubo.sliceY));
            glUniform4fv(wireSliceEnLoc, 1, glm::value_ptr(ubo.sliceEn));
            glUniform4fv(wireInvertLoc, 1, glm::value_ptr(ubo.invert));
            glUniform1f(wireClipEnabledLoc, ubo.point_clip.w);
            glDepthFunc(GL_LEQUAL); glDepthMask(GL_FALSE); glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(-1.0f,-1.0f); glDisable(GL_CULL_FACE);
            for (auto &e: edgeDrawList) if(e.second>0){ glBindVertexArray(e.first); glDrawElements(GL_LINES, e.second, GL_UNSIGNED_INT, 0); }
            glBindVertexArray(0); glDisable(GL_POLYGON_OFFSET_FILL);
        } else {
            glLineWidth(state.lineWidth); glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            glEnable(GL_POLYGON_OFFSET_LINE); glPolygonOffset(-1.0f,-1.0f);
            for (auto &e: edgeDrawList) if(e.second>0){ glBindVertexArray(e.first); glDrawElements(GL_LINES, e.second, GL_UNSIGNED_INT, 0); }
            glBindVertexArray(0); glDisable(GL_POLYGON_OFFSET_LINE); glLineWidth(1.0f);
        }
            ubo.meshColor_wire.w = 0.0f; glNamedBufferSubData(meshUbo, offsetof(MeshUBOData, meshColor_wire), sizeof(glm::vec4), &ubo.meshColor_wire);
        return;
    }
    if (state.showWireframe && !useCellEdges) {
        for (size_t di=0; di<drawList.size(); ++di) {
            glBindVertexArray(drawList[di].first);
            glLineWidth(state.lineWidth); glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            glEnable(GL_POLYGON_OFFSET_LINE); glPolygonOffset(-1.0f,-1.0f);
            ubo.meshColor_wire = glm::vec4(state.meshColor[0], state.meshColor[1], state.meshColor[2], 1.0f);
            glNamedBufferSubData(meshUbo, offsetof(MeshUBOData, meshColor_wire), sizeof(glm::vec4), &ubo.meshColor_wire);
            glDrawElements(GL_TRIANGLES, drawList[di].second, GL_UNSIGNED_INT, 0);
            glDisable(GL_POLYGON_OFFSET_LINE); glLineWidth(1.0f);
        ubo.meshColor_wire.w = 0.0f; glNamedBufferSubData(meshUbo, offsetof(MeshUBOData, meshColor_wire), sizeof(glm::vec4), &ubo.meshColor_wire);
        }
        glBindVertexArray(0); glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
}
