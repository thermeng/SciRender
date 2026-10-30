#pragma once

#include "cfd/cfd_types.h"

#include <QByteArray>
#include <QDataStream>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <functional>
#include <string>
#include <vector>

namespace cfd {
namespace detail {

// A single serialized field: its JSON name plus how to read it from a struct
// into JSON and how to write it back from JSON into the struct. One table per
// struct is shared by both serialize and deserialize, so a field added (or
// renamed) here is automatically mirrored in both directions -- the previous
// mirror-pair drift surface is closed.

template<typename Struct>
struct Field {
    const char* name;
    std::function<QJsonValue(const Struct&)> toJson;
    std::function<void(Struct&, const QJsonValue&)> fromJson;
};

// ---------- field factories (one per C++ type) ----------

// double: QJsonValue(double); toDouble(default=current).
template<typename Struct, typename T>
inline Field<Struct> dbl(const char* name, T Struct::*m) {
    return Field<Struct>{
        name,
        [m](const Struct& s) { return QJsonValue(s.*m); },
        [m](Struct& s, const QJsonValue& v) { s.*m = v.toDouble(s.*m); }};
}

// integral stored as qint64 (size_t): toVariant().toULongLong() (no default,
// matches legacy behaviour of 0 for a missing key).
template<typename Struct, typename T>
inline Field<Struct> num(const char* name, T Struct::*m) {
    return Field<Struct>{
        name,
        [m](const Struct& s) { return QJsonValue(static_cast<qint64>(s.*m)); },
        [m](Struct& s, const QJsonValue& v) {
            s.*m = static_cast<T>(v.toVariant().toULongLong());
        }};
}

// bool: toBool(default=current).
template<typename Struct, typename T>
inline Field<Struct> flg(const char* name, T Struct::*m) {
    return Field<Struct>{
        name,
        [m](const Struct& s) { return QJsonValue(s.*m); },
        [m](Struct& s, const QJsonValue& v) { s.*m = static_cast<T>(v.toBool(static_cast<bool>(s.*m))); }};
}

// int: toInt(default=current).
template<typename Struct, typename T>
inline Field<Struct> i32(const char* name, T Struct::*m) {
    return Field<Struct>{
        name,
        [m](const Struct& s) { return QJsonValue(static_cast<int>(s.*m)); },
        [m](Struct& s, const QJsonValue& v) { s.*m = static_cast<T>(v.toInt(static_cast<int>(s.*m))); }};
}

// enum stored as int: toInt(default=current) cast back to enum.
template<typename Struct, typename T>
inline Field<Struct> enm(const char* name, T Struct::*m) {
    return Field<Struct>{
        name,
        [m](const Struct& s) { return QJsonValue(static_cast<int>(s.*m)); },
        [m](Struct& s, const QJsonValue& v) { s.*m = static_cast<T>(v.toInt(static_cast<int>(s.*m))); }};
}

// std::string: from/to QString; toString(default=current).
template<typename Struct, typename T>
inline Field<Struct> str(const char* name, T Struct::*m) {
    return Field<Struct>{
        name,
        [m](const Struct& s) { return QJsonValue(QString::fromUtf8(s.*m)); },
        [m](Struct& s, const QJsonValue& v) {
            s.*m = v.toString(QString::fromUtf8(s.*m)).toStdString();
        }};
}

// ---------- generic object helpers ----------

template<typename Struct>
inline QJsonObject writeObject(const Struct& s,
                               const std::vector<Field<Struct>>& fields) {
    QJsonObject o;
    for (const auto& f : fields) o[QString::fromUtf8(f.name)] = f.toJson(s);
    return o;
}

template<typename Struct>
inline void readObject(Struct& s, const QJsonObject& o,
                       const std::vector<Field<Struct>>& fields) {
    for (const auto& f : fields) f.fromJson(s, o.value(QString::fromUtf8(f.name)));
}

// ---------- TransientSettings ----------

inline const std::vector<Field<TransientSettings>>& transientFields() {
    using F = detail::Field<TransientSettings>;
    static const std::vector<F> fields{
        flg("enabled", &TransientSettings::enabled),
        dbl("dt", &TransientSettings::dt),
        dbl("tEnd", &TransientSettings::tEnd),
        enm("timeScheme", &TransientSettings::timeScheme),
        flg("adaptiveCfl", &TransientSettings::adaptiveCfl),
        dbl("cfl", &TransientSettings::cfl),
        dbl("minDt", &TransientSettings::minDt),
        dbl("maxDt", &TransientSettings::maxDt),
        i32("writeInterval", &TransientSettings::writeInterval),
        i32("maxOuterCorrectors", &TransientSettings::maxOuterCorrectors),
        i32("nonOrthogonalCorrectors", &TransientSettings::nonOrthogonalCorrectors),
        str("pvdFile", &TransientSettings::pvdFile),
    };
    return fields;
}

inline QJsonObject serializeTransient(const TransientSettings& s) {
    return writeObject(s, transientFields());
}

inline void deserializeTransient(const QJsonObject& o, TransientSettings& s) {
    readObject(s, o, transientFields());
}

// ---------- PatchBC ----------

inline const std::vector<Field<PatchBC>>& patchBCFields() {
    using F = detail::Field<PatchBC>;
    static const std::vector<F> fields{
        str("patchName", &PatchBC::patchName),
        enm("type", &PatchBC::type),
        dbl("u", &PatchBC::u),
        dbl("v", &PatchBC::v),
        dbl("w", &PatchBC::w),
        dbl("pressure", &PatchBC::pressure),
        flg("suppressBackflow", &PatchBC::suppressBackflow),
        dbl("backflowU", &PatchBC::backflowU),
        dbl("backflowV", &PatchBC::backflowV),
        dbl("backflowW", &PatchBC::backflowW),
        dbl("backflowK", &PatchBC::backflowK),
        dbl("backflowOmega", &PatchBC::backflowOmega),
        dbl("kInlet", &PatchBC::kInlet),
        dbl("omegaInlet", &PatchBC::omegaInlet),
    };
    return fields;
}

// ---------- CaseSettings ----------

inline const std::vector<Field<CaseSettings>>& caseSettingsFields() {
    using F = detail::Field<CaseSettings>;
    static const std::vector<F> fields{
        dbl("rho", &CaseSettings::rho),
        dbl("nu", &CaseSettings::nu),
        dbl("tolerance", &CaseSettings::tolerance),
        num("maxIterations", &CaseSettings::maxIterations),
        flg("runFixedIterations", &CaseSettings::runFixedIterations),
        i32("convergenceHoldIterations", &CaseSettings::convergenceHoldIterations),
        dbl("alphaU", &CaseSettings::alphaU),
        dbl("alphaP", &CaseSettings::alphaP),
        flg("simplec", &CaseSettings::simplec),
        i32("scheme", &CaseSettings::scheme),
        i32("gradientScheme", &CaseSettings::gradientScheme),
        flg("rhieChow", &CaseSettings::rhieChow),
        flg("turbulence", &CaseSettings::turbulence),
        dbl("kInf", &CaseSettings::kInf),
        dbl("omegaInf", &CaseSettings::omegaInf),
        dbl("alphaK", &CaseSettings::alphaK),
        dbl("alphaOmega", &CaseSettings::alphaOmega),
        num("pressureRefCell", &CaseSettings::pressureRefCell),
        i32("pressureCorrectors", &CaseSettings::pressureCorrectors),
        flg("limitPressureCorrection", &CaseSettings::limitPressureCorrection),
        dbl("pressureCorrectionLimit", &CaseSettings::pressureCorrectionLimit),
        num("printFrequency", &CaseSettings::printFrequency),
        num("residualHistoryWindow", &CaseSettings::residualHistoryWindow),
        i32("numThreads", &CaseSettings::numThreads),
        flg("pressureUseAMG", &CaseSettings::pressureUseAMG),
        flg("pressureUseAMGPreconditioner", &CaseSettings::pressureUseAMGPreconditioner),
        num("pressureAmgThreshold", &CaseSettings::pressureAmgThreshold),
        flg("useAMGMomentum", &CaseSettings::useAMGMomentum),
        num("momentumAmgThreshold", &CaseSettings::momentumAmgThreshold),
        i32("momentumAmgCycles", &CaseSettings::momentumAmgCycles),
        dbl("momentumAmgMaxResidual", &CaseSettings::momentumAmgMaxResidual),
        flg("momentumSolverStats", &CaseSettings::momentumSolverStats),
        Field<CaseSettings>{
            "transient",
            [](const CaseSettings& s) { return serializeTransient(s.transient); },
            [](CaseSettings& s, const QJsonValue& v) {
                deserializeTransient(v.toObject(), s.transient);
            },
        },
    };
    return fields;
}

} // namespace detail

// Qt-based case settings/BC codec. Keeps the SCFD file format's JSON payload
// in one place so CfdController only handles file I/O and thread orchestration.
// Serialize and deserialize share a single field table (see detail::*), so a
// field added here is automatically mirrored in both directions.
inline QByteArray serializeCaseSettings(const CaseSettings& s,
                                        const std::vector<PatchBC>& patchBCs) {
    QJsonObject o = detail::writeObject(s, detail::caseSettingsFields());

    QJsonArray bcArr;
    for (const auto& pbc : patchBCs) {
        bcArr.append(detail::writeObject(pbc, detail::patchBCFields()));
    }
    o["patchBCs"] = bcArr;

    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

inline bool deserializeCaseSettings(const QByteArray& settings, CaseSettings& s,
                                    std::vector<PatchBC>& patchBCs) {
    QJsonDocument doc = QJsonDocument::fromJson(settings);
    if (!doc.isObject()) return false;
    const QJsonObject o = doc.object();

    detail::readObject(s, o, detail::caseSettingsFields());

    const QJsonArray bcArr = o.value("patchBCs").toArray();
    if (!bcArr.isEmpty()) {
        patchBCs.clear();
        for (const auto& v : bcArr) {
            cfd::PatchBC pbc;
            detail::readObject(pbc, v.toObject(), detail::patchBCFields());
            patchBCs.push_back(pbc);
        }
    }
    return true;
}

} // namespace cfd
