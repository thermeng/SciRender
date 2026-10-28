#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include "field/VectorField.h"
#include <nlohmann/json.hpp>
#include <cstddef>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <vector>
#include <iostream>

namespace fvm {

struct CheckpointConfig {
    std::size_t saveEvery = 0;
    std::size_t retainCount = 3;
    std::string file = "checkpoint.cpt";
    std::string restartFrom = "";
};

// Everything needed to resume a transient run at the correct physical time.
// Without this a restart silently replays from t=0 on top of a mid-run field,
// and the BDF2 history (u^n, u^{n-1}) is lost so the scheme drops to Euler.
template <std::size_t Dim>
struct TransientBlock {
    double time = 0.0;
    double dt = 0.0;
    double dtPrev = 0.0;   // 0 => next step uses explicit Euler
    std::size_t step = 0;

    VectorField<Dim>* velocityN = nullptr;
    VectorField<Dim>* velocityNM1 = nullptr;
    Field<Dim>* kN = nullptr;
    Field<Dim>* kNM1 = nullptr;
    Field<Dim>* omegaN = nullptr;
    Field<Dim>* omegaNM1 = nullptr;

    std::vector<std::string> pvdEntries;
};

class Checkpoint {
public:
    static constexpr unsigned int MAGIC = 0x434D5646;
    static constexpr unsigned int FORMAT_VERSION = 1;

    static void writeU32(std::ofstream& out, std::size_t val) {
        auto v = static_cast<unsigned int>(val);
        out.write(reinterpret_cast<const char*>(&v), sizeof(v));
    }

    static std::size_t readU32(std::ifstream& in) {
        unsigned int v = 0;
        in.read(reinterpret_cast<char*>(&v), sizeof(v));
        return static_cast<std::size_t>(v);
    }

    static std::string makeIndexedName(const std::string& base, std::size_t index) {
        return base + "." + std::to_string(index);
    }

    static void rotate(const CheckpointConfig& config) {
        if (config.retainCount == 0) return;
        std::string base = config.file;
        std::string oldest = makeIndexedName(base, config.retainCount);
        std::remove(oldest.c_str());
        for (std::size_t i = config.retainCount - 1; i >= 1; --i) {
            std::string from = makeIndexedName(base, i);
            std::string to = makeIndexedName(base, i + 1);
            std::rename(from.c_str(), to.c_str());
        }
        std::string first = makeIndexedName(base, 1);
        std::rename(base.c_str(), first.c_str());
    }

    template <std::size_t Dim>
    static bool save(const CheckpointConfig& config,
                     std::size_t iteration,
                     double contResid, double momResid,
                     double adaptiveAlphaP, std::size_t stallCount, double prevContResid,
                     const VectorField<Dim>& velocity,
                     const Field<Dim>& pressure,
                     const Field<Dim>* k = nullptr,
                     const Field<Dim>* omega = nullptr,
                     const Field<Dim>* nut = nullptr,
                     const TransientBlock<Dim>* transient = nullptr) {
        rotate(config);

        std::ofstream out(config.file, std::ios::binary);
        if (!out.is_open()) return false;

        const bool hasTransient = (transient != nullptr);
        // All-or-none contract: the fields list below describes exactly what
        // is written, so the reader never has to guess from pointer nullness.
        const bool hasTurbulence = (k != nullptr && omega != nullptr && nut != nullptr);
        const bool transientTurbulence = hasTransient &&
            transient->kN && transient->kNM1 &&
            transient->omegaN && transient->omegaNM1;

        std::ostringstream metadata;
        metadata << "{";
        metadata << "\"format_version\":" << FORMAT_VERSION << ",";
        metadata << "\"dimension\":" << Dim << ",";
        metadata << "\"n_cells\":" << pressure.size() << ",";
        metadata << "\"iteration\":" << iteration << ",";
        metadata << "\"cont_resid\":" << std::scientific << contResid << ",";
        metadata << "\"mom_resid\":" << std::scientific << momResid << ",";
        metadata << "\"adaptive_alpha_p\":" << std::scientific << adaptiveAlphaP << ",";
        metadata << "\"stall_count\":" << stallCount << ",";
        metadata << "\"prev_cont_resid\":" << std::scientific << prevContResid << ",";
        metadata << "\"turbulence_enabled\":" << (hasTurbulence ? "true" : "false") << ",";
        metadata << "\"has_transient\":" << (hasTransient ? "true" : "false") << ",";
        metadata << "\"transient_turbulence_enabled\":"
                 << (transientTurbulence ? "true" : "false") << ",";
        metadata << "\"fields\":[\"u\"";
        if constexpr (Dim >= 2) metadata << ",\"v\"";
        if constexpr (Dim >= 3) metadata << ",\"w\"";
        metadata << ",\"p\"";
        if (hasTurbulence) metadata << ",\"k\",\"omega\",\"nut\"";
        metadata << "]";
        metadata << "}";

        std::string metaStr = metadata.str();

        writeU32(out, MAGIC);
        writeU32(out, FORMAT_VERSION);
        writeU32(out, metaStr.size());
        out.write(metaStr.c_str(), static_cast<std::streamsize>(metaStr.size()));

        for (std::size_t d = 0; d < Dim; ++d)
            writeFieldData(out, velocity[d]);

        writeFieldData(out, pressure);

        if (hasTurbulence) {
            writeFieldData(out, *k);
            writeFieldData(out, *omega);
            writeFieldData(out, *nut);
        }

        if (hasTransient) {
            out.write(reinterpret_cast<const char*>(&transient->time), sizeof(double));
            out.write(reinterpret_cast<const char*>(&transient->dt), sizeof(double));
            out.write(reinterpret_cast<const char*>(&transient->dtPrev), sizeof(double));
            writeU32(out, transient->step);
            if (transient->velocityN)   for (std::size_t d = 0; d < Dim; ++d) writeFieldData(out, (*transient->velocityN)[d]);
            if (transient->velocityNM1) for (std::size_t d = 0; d < Dim; ++d) writeFieldData(out, (*transient->velocityNM1)[d]);
            if (transientTurbulence) {
                writeFieldData(out, *transient->kN);
                writeFieldData(out, *transient->kNM1);
                writeFieldData(out, *transient->omegaN);
                writeFieldData(out, *transient->omegaNM1);
            }
            writeU32(out, transient->pvdEntries.size());
            for (const auto& e : transient->pvdEntries) {
                writeU32(out, e.size());
                out.write(e.data(), static_cast<std::streamsize>(e.size()));
            }
        }

        return out.good();
    }

    template <std::size_t Dim>
    static bool load(const std::string& filename,
                     const Mesh<Dim>& mesh,
                     std::size_t& iteration,
                     double& contResid, double& momResid,
                     double& adaptiveAlphaP, std::size_t& stallCount, double& prevContResid,
                     VectorField<Dim>& velocity,
                     Field<Dim>& pressure,
                     Field<Dim>* k = nullptr,
                     Field<Dim>* omega = nullptr,
                     Field<Dim>* nut = nullptr,
                     TransientBlock<Dim>* transient = nullptr) {
        std::ifstream in(filename, std::ios::binary);
        if (!in.is_open()) return false;

        std::size_t magic = readU32(in);
        if (magic != MAGIC) return false;

        std::size_t version = readU32(in);
        if (version > FORMAT_VERSION) return false;

        std::size_t metaLen = readU32(in);
        std::string metaStr(metaLen, '\0');
        in.read(&metaStr[0], static_cast<std::streamsize>(metaLen));

        auto j = nlohmann::json::parse(metaStr);

        if (j.contains("n_cells") && j["n_cells"].get<std::size_t>() != mesh.nCells())
            return false;

        if (j.contains("iteration"))      iteration      = j["iteration"].get<std::size_t>();
        if (j.contains("cont_resid"))     contResid      = j["cont_resid"].get<double>();
        if (j.contains("mom_resid"))      momResid       = j["mom_resid"].get<double>();
        if (j.contains("adaptive_alpha_p")) adaptiveAlphaP = j["adaptive_alpha_p"].get<double>();
        if (j.contains("stall_count"))    stallCount     = j["stall_count"].get<std::size_t>();
        if (j.contains("prev_cont_resid")) prevContResid = j["prev_cont_resid"].get<double>();

        std::size_t nCells = mesh.nCells();

        for (std::size_t d = 0; d < Dim; ++d) {
            if (!readFieldData(in, velocity[d], nCells)) return false;
        }

        if (!readFieldData(in, pressure, nCells)) return false;

        // Consume the remaining fields in the order the writer recorded them.
        // Deciding from pointer nullness desynchronised the stream whenever a
        // checkpoint written with turbulence was loaded without it (or the
        // reverse): the transient block was then read from the middle of a
        // field and produced garbage or a bogus "no transient state".
        auto consumeTurbulenceField = [&](const std::string& name) -> bool {
            Field<Dim>* dst = nullptr;
            if (name == "k") dst = k;
            else if (name == "omega") dst = omega;
            else if (name == "nut") dst = nut;
            else return true; // u/v/w/p are consumed above
            if (dst != nullptr && dst->size() == nCells)
                return readFieldData(in, *dst, nCells);
            return skipFieldData(in, nCells);
        };

        std::vector<std::string> fields;
        if (j.contains("fields") && j["fields"].is_array()) {
            for (const auto& name : j["fields"]) {
                if (name.is_string()) fields.push_back(name.get<std::string>());
            }
        }
        if (!fields.empty()) {
            for (const auto& name : fields) {
                if (name == "u" || name == "v" || name == "w" || name == "p") continue;
                if (!consumeTurbulenceField(name)) return false;
            }
        } else if (j.value("turbulence_enabled", false)) {
            // Legacy file without a field list.
            for (const char* name : {"k", "omega", "nut"}) {
                if (!consumeTurbulenceField(name)) return false;
            }
        }

        if (transient != nullptr && j.value("has_transient", false)) {
            if (!readDouble(in, transient->time))   return false;
            if (!readDouble(in, transient->dt))     return false;
            if (!readDouble(in, transient->dtPrev)) return false;
            transient->step = readU32(in);
            if (transient->velocityN) {
                for (std::size_t d = 0; d < Dim; ++d)
                    if (!readFieldData(in, (*transient->velocityN)[d], nCells)) return false;
            }
            if (transient->velocityNM1) {
                for (std::size_t d = 0; d < Dim; ++d)
                    if (!readFieldData(in, (*transient->velocityNM1)[d], nCells)) return false;
            }
            // Same metadata-driven rule for the transient turbulence history.
            const bool fileTransientTurbulence = j.value(
                "transient_turbulence_enabled",
                j.value("turbulence_enabled", false));
            if (fileTransientTurbulence) {
                auto consumeTB = [&](Field<Dim>* dst) -> bool {
                    return dst ? readFieldData(in, *dst, nCells)
                               : skipFieldData(in, nCells);
                };
                if (!consumeTB(transient->kN))       return false;
                if (!consumeTB(transient->kNM1))     return false;
                if (!consumeTB(transient->omegaN))   return false;
                if (!consumeTB(transient->omegaNM1)) return false;
            }

            transient->pvdEntries.clear();
            const std::size_t nEntries = readU32(in);
            for (std::size_t i = 0; i < nEntries; ++i) {
                const std::size_t len = readU32(in);
                std::string entry(len, '\0');
                if (len > 0) in.read(&entry[0], static_cast<std::streamsize>(len));
                if (!in.good()) return false;
                transient->pvdEntries.push_back(std::move(entry));
            }
        }

        return in.good() || in.eof();
    }

private:
    static bool readDouble(std::ifstream& in, double& out) {
        in.read(reinterpret_cast<char*>(&out), sizeof(double));
        return in.good();
    }

    template<std::size_t Dim>
    static void writeFieldData(std::ofstream& out, const Field<Dim>& field) {
        const auto vals = field.values();
        out.write(reinterpret_cast<const char*>(vals.data()),
                  static_cast<std::streamsize>(vals.size() * sizeof(double)));
    }

    template<std::size_t Dim>
    static bool readFieldData(std::ifstream& in, Field<Dim>& field, std::size_t expectedSize) {
        auto vals = field.values();
        if (vals.size() != expectedSize) return false;
        in.read(reinterpret_cast<char*>(vals.data()),
                static_cast<std::streamsize>(expectedSize * sizeof(double)));
        return in.good();
    }

    // Consume a field the caller did not ask for, keeping the stream aligned.
    static bool skipFieldData(std::ifstream& in, std::size_t nCells) {
        in.ignore(static_cast<std::streamsize>(nCells * sizeof(double)));
        return in.good() || in.eof();
    }
};

} // namespace fvm
