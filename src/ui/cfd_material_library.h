#pragma once

#include "cfd/material.h"

#include <QSettings>
#include <QString>
#include <QVector>
#include <optional>

namespace cfdUi {

// Persistent library of user-defined materials on top of the built-in
// reference materials (cfd::materials::builtIns). Stored in QSettings so the
// entries survive across cases and sessions. The settings scope is
// injectable so tests can use a throwaway branch instead of the user's real
// settings.
class MaterialLibrary {
public:
    explicit MaterialLibrary(QString org = QStringLiteral("SciRender"),
                             QString app = QStringLiteral("SciRender"));

    QVector<cfd::Material> userMaterials() const;

    // Add or overwrite by name. Returns false (without storing) for empty
    // names, built-in names, the "Custom" sentinel, or non-positive /
    // non-finite properties.
    bool saveUserMaterial(const cfd::Material& m);

    // Returns false when no user material carries that name.
    bool removeUserMaterial(const QString& name);

    // Built-ins first, then user materials in storage order.
    QVector<cfd::Material> allMaterials() const;

    // Resolves built-ins and user materials; "Custom" and unknown names
    // return nullopt (callers fall back to hand-typed properties).
    std::optional<cfd::Material> find(const QString& name) const;

private:
    QSettings settings() const;
    QString m_org;
    QString m_app;
};

} // namespace cfdUi
