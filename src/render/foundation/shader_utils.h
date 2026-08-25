#pragma once

#include <glad/gl.h>
#include <iostream>
#include <string>

// Inserts a shared GLSL snippet right after the #version line so consuming
// fragment shaders keep their own version directive and uniform declarations.
inline std::string injectPbrCommon(const char* fragSrc, const std::string& common) {
    if (common.empty()) return std::string(fragSrc);
    std::string src(fragSrc);
    const size_t pos = src.find('\n');
    src.insert(pos == std::string::npos ? 0 : pos + 1, common);
    return src;
}

inline GLuint compileProgramWithGS(const char* vertSrc, const char* geoSrc,
                                   const char* fragSrc, const char* label) {
    auto compile = [&label](GLuint type, const char* src) -> GLuint {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            std::cerr << "[SHADER COMPILE ERROR " << label << "] " << log << std::endl;
            glDeleteShader(s);
            return 0;
        }
        return s;
    };

    GLuint vs = compile(GL_VERTEX_SHADER, vertSrc);
    if (!vs) return 0;

    GLuint gs = 0;
    if (geoSrc) {
        gs = compile(GL_GEOMETRY_SHADER, geoSrc);
        if (!gs) { glDeleteShader(vs); return 0; }
    }

    GLuint fs = compile(GL_FRAGMENT_SHADER, fragSrc);
    if (!fs) {
        glDeleteShader(vs);
        if (gs) glDeleteShader(gs);
        return 0;
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    if (gs) glAttachShader(prog, gs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    if (gs) glDeleteShader(gs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        std::cerr << "[SHADER LINK ERROR " << label << "] " << log << std::endl;
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

inline GLuint compileProgram(const char* vertSrc, const char* fragSrc, const char* label) {
    return compileProgramWithGS(vertSrc, nullptr, fragSrc, label);
}
