#include "render/settings/render_settings.h"

void RenderSettings::applyLoadedMesh(const std::shared_ptr<const RenderMesh>& loaded, const QString& name, bool resetCamera) {
    if (!loaded || loaded->vertices.empty()) { setStatus("Could not load result"); return; }
    // Set the new mesh first so that clearPostFields()'s meshDataUpdated()
    // emission (and any intermediate refreshFields() in the UI pages) sees the
    // correct loadedMesh instead of the previous/stale one.
    m_meshData.loadedMesh = loaded;
    clearPostFields();
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
// Preserve the user's current scalar field when it still resolves in the new
// mesh (including derived [Magnitude]/[X]/[Y]/[Z] fields); only fall back to
// the mesh's default scalar (or another available one) when it does not.
const std::string requestedScalar = m_state.activeScalarName;
const std::string resolvedScalar = FieldStore::resolveActiveScalar(*loaded, requestedScalar,
    FieldStore::toPlacement(m_state.scalarPlacement), m_state.computed.get());
if (!resolvedScalar.empty()) m_state.activeScalarName = resolvedScalar;
else if (!loaded->scalarName.empty()) m_state.activeScalarName = loaded->scalarName;
else {
    auto avail = FieldStore::availableScalars(*loaded, m_state.computed.get());
    if (!avail.empty()) m_state.activeScalarName = avail.front();
}
m_state.meshHasScalars = !loaded->scalars.empty() || FieldStore::hasAnyScalar(*loaded, m_state.computed.get());

// Auto-select placement: prefer Vertex (point data) when it exists, since
// surface shading uploads point-sized data; fall back to Cell Center only
// when there is no point counterpart. Recompute the range so the colorbar
// matches the data actually uploaded to the GPU.
if (loaded->attributes) {
    bool hasCell = loaded->attributes->cellScalars.find(m_state.activeScalarName) != loaded->attributes->cellScalars.end()
                   && !loaded->attributes->cellScalars.at(m_state.activeScalarName).empty();
    bool hasPoint = loaded->attributes->pointScalars.find(m_state.activeScalarName) != loaded->attributes->pointScalars.end()
                    && !loaded->attributes->pointScalars.at(m_state.activeScalarName).empty();
    if (hasPoint) m_state.scalarPlacement = 0;
    else if (hasCell) m_state.scalarPlacement = 1;
    else m_state.scalarPlacement = 0;
}
recomputeScalarRange();
m_meshData.guiMeta.scalarName = m_state.activeScalarName;

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

    // Hand the immutable payload to the render thread. Reframe the camera only
    // when the data itself changed (mesh load / case load / run completion); a
    // cancelled run visualises its partial result without yanking the view.
    m_renderer.setPendingMesh(loaded);
    if (resetCamera) resetCameraInstant();

    // Upload the resolved active scalar (not just mesh->scalars, which may be
    // the parser default) so surface/volume shading matches the selection
    // immediately after a mesh/run load instead of waiting for a placement toggle.
    {
        float mn = 0.f, mx = 1.f;
        auto snap = m_state.computed.get();
        auto place = m_state.scalarPlacement == 1 ? FieldStore::Placement::CellCenter : FieldStore::Placement::Vertex;
        const std::vector<float>* data = FieldStore::scalarData(*loaded, m_state.activeScalarName, mn, mx, place, snap);
        if (data && !data->empty()) {
            std::shared_ptr<const std::vector<float>> payload;
            if (data->size() == loaded->vertices.size() / 3
                || loaded->vertexSourceIndex.empty()
                || loaded->vertexSourceIndex.size() != loaded->vertices.size() / 3) {
                payload = std::make_shared<const std::vector<float>>(*data);
            } else if (data->size() == (size_t)loaded->sourcePointCount
                       || data->size() == (size_t)(loaded->gridDimX * loaded->gridDimY * loaded->gridDimZ)) {
                auto expanded = std::make_shared<std::vector<float>>();
                expanded->resize(loaded->vertices.size() / 3);
                const auto& lut = loaded->vertexSourceIndex;
                for (size_t i = 0; i < lut.size(); ++i) {
                    uint32_t src = lut[i];
                    if (src < data->size()) (*expanded)[i] = (*data)[src];
                    else (*expanded)[i] = (*data)[0];
                }
                payload = expanded;
            } else {
                payload = std::make_shared<const std::vector<float>>(*data);
            }
            m_meshData.guiMeta.scalars = *payload;
            m_renderer.markScalarDirty(payload);
            m_renderer.markVolumeDirty(loaded);
        }
    }

    // Keep the Isosurface page in sync after a run/case load (the mesh may not
    // have changed, so nothing else would refresh it).
    m_isoController.setCurrentMesh(loaded, m_state.isosurfaceField, m_state.isosurfacePlacement,
                                   m_state.computed);
    if (m_state.showIsosurface) m_isoController.recompute();

    // Volume/slice: build the derived voxel grid for an unstructured 3D mesh
    // (asynchronously; the renderer re-scatters the field into it each frame).
    m_derivedVolumeController.setCurrentMesh(loaded);

    emit meshLoadStateChanged();
    markStateDirty(); emit meshDataUpdated();
}