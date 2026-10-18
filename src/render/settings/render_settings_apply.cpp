#include "render/settings/render_settings.h"

void RenderSettings::applyLoadedMesh(const std::shared_ptr<const RenderMesh>& loaded, const QString& name) {
    if (!loaded || loaded->vertices.empty()) { setStatus("Could not load result"); return; }
    clearPostFields();
    m_meshData.loadedMesh = loaded;
    m_meshData.guiMeta = *loaded;
    m_meshData.guiMeta.vertices.clear(); m_meshData.guiMeta.vertices.shrink_to_fit();
    m_meshData.guiMeta.normals.clear(); m_meshData.guiMeta.normals.shrink_to_fit();
    m_meshData.guiMeta.indices.clear(); m_meshData.guiMeta.indices.shrink_to_fit();
    m_meshData.guiMeta.pointVectorsData.clear(); m_meshData.guiMeta.pointVectorsData.shrink_to_fit();
    m_meshData.guiMeta.attributes.reset();

    m_state.worldMinX = loaded->bounds.minX; m_state.worldMaxX = loaded->bounds.maxX;
    m_state.worldMinY = loaded->bounds.minY; m_state.worldMaxY = loaded->bounds.maxY;
    m_state.worldMinZ = loaded->bounds.minZ; m_state.worldMaxZ = loaded->bounds.maxZ;
    m_state.worldCenterX = loaded->bounds.centerX;
    m_state.worldCenterY = loaded->bounds.centerY;
    m_state.worldCenterZ = loaded->bounds.centerZ;
    m_state.worldRadius  = loaded->bounds.worldRadius;

    m_meshData.fileName = name.toStdString();
    m_meshData.triangleCount = static_cast<int>(loaded->indices.size() / 3);
    m_meshData.pointCount = loaded->sourcePointCount >= 0 ? loaded->sourcePointCount : static_cast<int>(loaded->vertices.size() / 3);
    m_meshData.datasetType = loaded->datasetType;
    m_meshData.meshFormat = loaded->fileFormat;
    m_state.hasMeshLoaded = true;
m_state.activeScalarName = loaded->scalarName;
m_state.meshHasScalars = !loaded->scalars.empty();

// Auto-select placement: prefer Vertex (point data) when it exists, since
// surface shading uploads point-sized data; fall back to Cell Center only
// when there is no point counterpart. Recompute the range so the colorbar
// matches the data actually uploaded to the GPU.
if (loaded->attributes) {
    bool hasCell = loaded->attributes->cellScalars.find(m_state.activeScalarName) != loaded->attributes->cellScalars.end();
    bool hasPoint = loaded->attributes->pointScalars.find(m_state.activeScalarName) != loaded->attributes->pointScalars.end();
    if (hasPoint) m_state.scalarPlacement = 0;
    else if (hasCell) m_state.scalarPlacement = 1;
    else m_state.scalarPlacement = 0;
}
recomputeScalarRange();

    m_state.meshHasVectors = !loaded->pointVectorsData.empty();
    m_state.meshHasCellVectors = !loaded->cellVectorsData.empty();
    if (!m_state.meshHasCellVectors && m_state.vectorPlacement == 1) m_state.vectorPlacement = 0;
    else if (!m_state.meshHasVectors && m_state.meshHasCellVectors) m_state.vectorPlacement = 1;
    if (!m_state.meshHasVectors) m_state.showStreamlines = false;

    if (!loaded->vectorName.empty()) {
        m_meshData.guiMeta.vectorName = loaded->vectorName;
        m_state.vectorField = loaded->vectorName;
        m_state.streamlineVectorField = loaded->vectorName;
    } else if (!loaded->cellVectorName.empty()) {
        m_meshData.guiMeta.vectorName = loaded->cellVectorName;
        m_state.vectorField = loaded->cellVectorName;
        m_state.streamlineVectorField = loaded->cellVectorName;
    }

    if (m_postVelocitySource.empty() && !loaded->availableVectorNames.empty())
        m_postVelocitySource = loaded->availableVectorNames.front();
    if (m_postPressureSource.empty() && !loaded->availableScalarNames.empty())
        m_postPressureSource = loaded->availableScalarNames.front();

    // Hand the immutable payload to the render thread and recenter the camera.
    m_renderer.setPendingMesh(loaded);
    resetCameraInstant();
    emit meshLoadStateChanged();
    markStateDirty(); emit meshDataUpdated();
}