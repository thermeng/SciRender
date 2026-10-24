#pragma once

#include "cfd/cfd_types.h"

#include <QByteArray>
#include <QDataStream>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

namespace cfd {

// Qt-based case settings/BC codec. Keeps the SCFD file format's JSON payload
// in one place so CfdController only handles file I/O and thread orchestration.
inline QByteArray serializeCaseSettings(const CaseSettings& s, const std::vector<PatchBC>& patchBCs) {
    QJsonObject o;
    o["rho"] = s.rho;
    o["nu"] = s.nu;
    o["tolerance"] = s.tolerance;
    o["maxIterations"] = (qint64)s.maxIterations;
    o["runFixedIterations"] = s.runFixedIterations;
    o["convergenceHoldIterations"] = s.convergenceHoldIterations;
    o["alphaU"] = s.alphaU;
    o["alphaP"] = s.alphaP;
    o["simplec"] = s.simplec;
    o["scheme"] = s.scheme;
    o["gradientScheme"] = s.gradientScheme;
    o["rhieChow"] = s.rhieChow;
    o["turbulence"] = s.turbulence;
    o["kInf"] = s.kInf;
    o["omegaInf"] = s.omegaInf;
    o["alphaK"] = s.alphaK;
    o["alphaOmega"] = s.alphaOmega;
    o["pressureRefCell"] = (qint64)s.pressureRefCell;
    o["pressureCorrectors"] = s.pressureCorrectors;
    o["limitPressureCorrection"] = s.limitPressureCorrection;
    o["pressureCorrectionLimit"] = s.pressureCorrectionLimit;
    o["printFrequency"] = (qint64)s.printFrequency;
    o["residualHistoryWindow"] = (qint64)s.residualHistoryWindow;
    o["numThreads"] = s.numThreads;
    o["pressureUseAMG"] = s.pressureUseAMG;
    o["pressureUseAMGPreconditioner"] = s.pressureUseAMGPreconditioner;
    o["pressureAmgThreshold"] = (qint64)s.pressureAmgThreshold;
    o["useAMGMomentum"] = s.useAMGMomentum;
    o["momentumAmgThreshold"] = (qint64)s.momentumAmgThreshold;
    o["momentumAmgCycles"] = s.momentumAmgCycles;
    o["momentumAmgMaxResidual"] = s.momentumAmgMaxResidual;
    o["momentumSolverStats"] = s.momentumSolverStats;

    QJsonObject t;
    t["enabled"] = s.transient.enabled;
    t["dt"] = s.transient.dt;
    t["tEnd"] = s.transient.tEnd;
    t["timeScheme"] = (int)s.transient.timeScheme;
    t["adaptiveCfl"] = s.transient.adaptiveCfl;
    t["cfl"] = s.transient.cfl;
    t["minDt"] = s.transient.minDt;
    t["maxDt"] = s.transient.maxDt;
    t["writeInterval"] = s.transient.writeInterval;
    t["maxOuterCorrectors"] = s.transient.maxOuterCorrectors;
    t["nonOrthogonalCorrectors"] = s.transient.nonOrthogonalCorrectors;
    t["pvdFile"] = QString::fromStdString(s.transient.pvdFile);
    o["transient"] = t;

    QJsonArray bcArr;
    for (const auto& pbc : patchBCs) {
        QJsonObject b;
        b["patchName"] = QString::fromStdString(pbc.patchName);
        b["type"] = (int)pbc.type;
        b["u"] = pbc.u; b["v"] = pbc.v; b["w"] = pbc.w;
        b["pressure"] = pbc.pressure;
        b["suppressBackflow"] = pbc.suppressBackflow;
        b["backflowU"] = pbc.backflowU; b["backflowV"] = pbc.backflowV; b["backflowW"] = pbc.backflowW;
        b["backflowK"] = pbc.backflowK; b["backflowOmega"] = pbc.backflowOmega;
        b["kInlet"] = pbc.kInlet; b["omegaInlet"] = pbc.omegaInlet;
        bcArr.append(b);
    }
    o["patchBCs"] = bcArr;

    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

inline bool deserializeCaseSettings(const QByteArray& settings, CaseSettings& s, std::vector<PatchBC>& patchBCs) {
    QJsonDocument doc = QJsonDocument::fromJson(settings);
    if (!doc.isObject()) return false;
    const QJsonObject o = doc.object();

    s.rho = o.value("rho").toDouble(s.rho);
    s.nu = o.value("nu").toDouble(s.nu);
    s.tolerance = o.value("tolerance").toDouble(s.tolerance);
    s.maxIterations = (std::size_t)o.value("maxIterations").toVariant().toULongLong();
    s.runFixedIterations = o.value("runFixedIterations").toBool(s.runFixedIterations);
    s.convergenceHoldIterations = o.value("convergenceHoldIterations").toInt(s.convergenceHoldIterations);
    s.alphaU = o.value("alphaU").toDouble(s.alphaU);
    s.alphaP = o.value("alphaP").toDouble(s.alphaP);
    s.simplec = o.value("simplec").toBool(s.simplec);
    s.scheme = o.value("scheme").toInt(s.scheme);
    s.gradientScheme = o.value("gradientScheme").toInt(s.gradientScheme);
    s.rhieChow = o.value("rhieChow").toBool(s.rhieChow);
    s.turbulence = o.value("turbulence").toBool(s.turbulence);
    s.kInf = o.value("kInf").toDouble(s.kInf);
    s.omegaInf = o.value("omegaInf").toDouble(s.omegaInf);
    s.alphaK = o.value("alphaK").toDouble(s.alphaK);
    s.alphaOmega = o.value("alphaOmega").toDouble(s.alphaOmega);
    s.pressureRefCell = (std::size_t)o.value("pressureRefCell").toVariant().toULongLong();
    s.pressureCorrectors = o.value("pressureCorrectors").toInt(s.pressureCorrectors);
    s.limitPressureCorrection = o.value("limitPressureCorrection").toBool(s.limitPressureCorrection);
    s.pressureCorrectionLimit = o.value("pressureCorrectionLimit").toDouble(s.pressureCorrectionLimit);
    s.printFrequency = (std::size_t)o.value("printFrequency").toVariant().toULongLong();
    s.residualHistoryWindow = (std::size_t)o.value("residualHistoryWindow").toVariant().toULongLong();
    s.numThreads = o.value("numThreads").toInt(s.numThreads);
    s.pressureUseAMG = o.value("pressureUseAMG").toBool(s.pressureUseAMG);
    s.pressureUseAMGPreconditioner = o.value("pressureUseAMGPreconditioner").toBool(s.pressureUseAMGPreconditioner);
    s.pressureAmgThreshold = (std::size_t)o.value("pressureAmgThreshold").toVariant().toULongLong();
    s.useAMGMomentum = o.value("useAMGMomentum").toBool(s.useAMGMomentum);
    s.momentumAmgThreshold = (std::size_t)o.value("momentumAmgThreshold").toVariant().toULongLong();
    s.momentumAmgCycles = o.value("momentumAmgCycles").toInt(s.momentumAmgCycles);
    s.momentumAmgMaxResidual = o.value("momentumAmgMaxResidual").toDouble(s.momentumAmgMaxResidual);
    s.momentumSolverStats = o.value("momentumSolverStats").toBool(s.momentumSolverStats);

    const QJsonObject t = o.value("transient").toObject();
    s.transient.enabled = t.value("enabled").toBool(s.transient.enabled);
    s.transient.dt = t.value("dt").toDouble(s.transient.dt);
    s.transient.tEnd = t.value("tEnd").toDouble(s.transient.tEnd);
    s.transient.timeScheme = (cfd::TransientSettings::TimeScheme)t.value("timeScheme").toInt((int)s.transient.timeScheme);
    s.transient.adaptiveCfl = t.value("adaptiveCfl").toBool(s.transient.adaptiveCfl);
    s.transient.cfl = t.value("cfl").toDouble(s.transient.cfl);
    s.transient.minDt = t.value("minDt").toDouble(s.transient.minDt);
    s.transient.maxDt = t.value("maxDt").toDouble(s.transient.maxDt);
    s.transient.writeInterval = t.value("writeInterval").toInt(s.transient.writeInterval);
    s.transient.maxOuterCorrectors = t.value("maxOuterCorrectors").toInt(s.transient.maxOuterCorrectors);
    s.transient.nonOrthogonalCorrectors = t.value("nonOrthogonalCorrectors").toInt(s.transient.nonOrthogonalCorrectors);
    s.transient.pvdFile = t.value("pvdFile").toString(QString::fromStdString(s.transient.pvdFile)).toStdString();

    const QJsonArray bcArr = o.value("patchBCs").toArray();
    if (!bcArr.isEmpty()) {
        patchBCs.clear();
        for (const auto& v : bcArr) {
            const QJsonObject b = v.toObject();
            cfd::PatchBC pbc;
            pbc.patchName = b.value("patchName").toString().toStdString();
            pbc.type = static_cast<cfd::BCType>(b.value("type").toInt((int)cfd::BCType::WallNoSlip));
            pbc.u = b.value("u").toDouble(pbc.u);
            pbc.v = b.value("v").toDouble(pbc.v);
            pbc.w = b.value("w").toDouble(pbc.w);
            pbc.pressure = b.value("pressure").toDouble(pbc.pressure);
            pbc.suppressBackflow = b.value("suppressBackflow").toBool(pbc.suppressBackflow);
            pbc.backflowU = b.value("backflowU").toDouble(pbc.backflowU);
            pbc.backflowV = b.value("backflowV").toDouble(pbc.backflowV);
            pbc.backflowW = b.value("backflowW").toDouble(pbc.backflowW);
            pbc.backflowK = b.value("backflowK").toDouble(pbc.backflowK);
            pbc.backflowOmega = b.value("backflowOmega").toDouble(pbc.backflowOmega);
            pbc.kInlet = b.value("kInlet").toDouble(pbc.kInlet);
            pbc.omegaInlet = b.value("omegaInlet").toDouble(pbc.omegaInlet);
            patchBCs.push_back(pbc);
        }
    }
    return true;
}

} // namespace cfd