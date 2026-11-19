#include "ui/cfd_material_library.h"

#include <algorithm>
#include <cmath>

namespace cfdUi {

namespace {
constexpr const char* kGroup = "cfd/userMaterials";

bool validProperties(const cfd::Material& m) {
    return std::isfinite(m.rho) && std::isfinite(m.mu) && m.rho > 0.0 && m.mu > 0.0;
}
} // namespace

MaterialLibrary::MaterialLibrary(QString org, QString app)
    : m_org(std::move(org)), m_app(std::move(app)) {}

QSettings MaterialLibrary::settings() const { return QSettings(m_org, m_app); }

QVector<cfd::Material> MaterialLibrary::userMaterials() const {
    QVector<cfd::Material> out;
    QSettings s = settings();
    const int n = s.beginReadArray(QString::fromUtf8(kGroup));
    for (int i = 0; i < n; ++i) {
        s.setArrayIndex(i);
        cfd::Material m;
        m.name = s.value("name").toString().toStdString();
        m.rho = s.value("rho").toDouble();
        m.mu = s.value("mu").toDouble();
        if (!m.name.empty() && validProperties(m)) out.push_back(std::move(m));
    }
    s.endArray();
    return out;
}

bool MaterialLibrary::saveUserMaterial(const cfd::Material& m) {
    const QString name = QString::fromStdString(m.name).trimmed();
    if (name.isEmpty() || !validProperties(m)) return false;
    if (name.compare(QLatin1String(cfd::materials::customName()), Qt::CaseInsensitive) == 0)
        return false;
    for (const auto& b : cfd::materials::builtIns()) {
        if (name.compare(QString::fromStdString(b.name), Qt::CaseInsensitive) == 0)
            return false;
    }
    QVector<cfd::Material> all = userMaterials();
    cfd::Material entry = m;
    entry.name = name.toStdString();
    bool replaced = false;
    for (auto& existing : all) {
        if (QString::fromStdString(existing.name).compare(name, Qt::CaseInsensitive) == 0) {
            existing = entry;
            replaced = true;
            break;
        }
    }
    if (!replaced) all.push_back(entry);

    QSettings s = settings();
    s.remove(QString::fromUtf8(kGroup)); // drop stale array entries on shrink
    s.beginWriteArray(QString::fromUtf8(kGroup));
    for (int i = 0; i < all.size(); ++i) {
        s.setArrayIndex(i);
        s.setValue("name", QString::fromStdString(all[i].name));
        s.setValue("rho", all[i].rho);
        s.setValue("mu", all[i].mu);
    }
    s.endArray();
    return true;
}

bool MaterialLibrary::removeUserMaterial(const QString& name) {
    QVector<cfd::Material> all = userMaterials();
    const int before = all.size();
    all.erase(std::remove_if(all.begin(), all.end(),
                             [&](const cfd::Material& m) {
                                 return QString::fromStdString(m.name)
                                     .compare(name, Qt::CaseInsensitive) == 0;
                             }),
              all.end());
    if (all.size() == before) return false;

    QSettings s = settings();
    s.remove(QString::fromUtf8(kGroup));
    s.beginWriteArray(QString::fromUtf8(kGroup));
    for (int i = 0; i < all.size(); ++i) {
        s.setArrayIndex(i);
        s.setValue("name", QString::fromStdString(all[i].name));
        s.setValue("rho", all[i].rho);
        s.setValue("mu", all[i].mu);
    }
    s.endArray();
    return true;
}

QVector<cfd::Material> MaterialLibrary::allMaterials() const {
    QVector<cfd::Material> out;
    for (const auto& b : cfd::materials::builtIns()) out.push_back(b);
    const auto users = userMaterials();
    for (const auto& u : users) out.push_back(u);
    return out;
}

std::optional<cfd::Material> MaterialLibrary::find(const QString& name) const {
    if (name.isEmpty()) return std::nullopt;
    for (const auto& m : allMaterials()) {
        if (QString::fromStdString(m.name).compare(name, Qt::CaseInsensitive) == 0)
            return m;
    }
    return std::nullopt;
}

} // namespace cfdUi
