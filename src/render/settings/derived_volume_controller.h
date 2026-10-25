#pragma once

#include <QObject>
#include <QFutureWatcher>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "render/foundation/renderer.h"
#include "core/mesh_loader.h"
#include "core/unstructured_volume.h"

// DerivedVolumeController — builds the voxel grid that volume rendering and
// axis-aligned slice planes need for an unstructured 3D mesh.
//
// The grid is geometry-only: it changes only with the topology, so transient
// playback (scalars advance per frame) keeps the built grid and the renderer
// only re-scatters the field into it. The build runs on QtConcurrent and hands
// the finished shared_ptr to the renderer through the same zero-copy pending
// slot the mesh/isosurface paths use.
class DerivedVolumeController : public QObject {
    Q_OBJECT
public:
    explicit DerivedVolumeController(Renderer& renderer, QObject* parent = nullptr);

    // Rebuilds when the topology changed; a repeated call with the same mesh
    // (e.g. a scalar-only frame) is a no-op.
    void setCurrentMesh(std::shared_ptr<const RenderMesh> mesh);
    void clear();

private slots:
    void onBuilt();

signals:
    void built();

private:
    struct BuildResult {
        std::shared_ptr<const volume::DerivedGrid> grid;
        uint64_t token = 0;
    };

    void launch();
    static std::uint64_t topologySignature(const RenderMesh& mesh);

    Renderer& m_renderer;
    std::shared_ptr<const RenderMesh> m_mesh;
    QFutureWatcher<BuildResult> m_watcher;
    uint64_t m_token = 0;       // bumped for every launch / clear
    uint64_t m_signature = 0;   // topology of m_mesh, 0 = not eligible
};
