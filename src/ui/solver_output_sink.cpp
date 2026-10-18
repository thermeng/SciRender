#include "solver_output_sink.h"

#include <QMetaObject>

SolverOutputSink::SolverOutputSink(QObject* parent) : QObject(parent) {}

SolverOutputSink::~SolverOutputSink() {
    uninstall();
}

void SolverOutputSink::install() {
    if (m_coutBuf) return;
    m_oldCout = std::cout.rdbuf();
    m_oldCerr = std::cerr.rdbuf();
    m_coutBuf = new TeeBuf(m_oldCout, this);
    m_cerrBuf = new TeeBuf(m_oldCerr, this);
    std::cout.rdbuf(m_coutBuf);
    std::cerr.rdbuf(m_cerrBuf);
}

void SolverOutputSink::uninstall() {
    if (!m_coutBuf) return;
    std::cout.rdbuf(m_oldCout);
    std::cerr.rdbuf(m_oldCerr);
    delete m_coutBuf;
    delete m_cerrBuf;
    m_coutBuf = nullptr;
    m_cerrBuf = nullptr;
}

void SolverOutputSink::pushText(const QString& t) {
    emit text(t);
}

SolverOutputSink::TeeBuf::TeeBuf(std::streambuf* orig, SolverOutputSink* sink)
    : m_orig(orig), m_sink(sink) {}

int SolverOutputSink::TeeBuf::overflow(int c) {
    if (c == EOF) return 0;
    char ch = static_cast<char>(c);
    m_buf.push_back(ch);
    if (m_orig) m_orig->sputc(ch);
    if (ch == '\n') {
        emitLine(QString::fromUtf8(m_buf.c_str(), static_cast<int>(m_buf.size())));
        m_buf.clear();
    }
    return c;
}

std::streamsize SolverOutputSink::TeeBuf::xsputn(const char* s, std::streamsize n) {
    if (m_orig) m_orig->sputn(s, n);
    m_buf.append(s, static_cast<std::size_t>(n));
    std::size_t pos = 0;
    while ((pos = m_buf.find('\n')) != std::string::npos) {
        emitLine(QString::fromUtf8(m_buf.c_str(), static_cast<int>(pos + 1)));
        m_buf.erase(0, pos + 1);
    }
    return n;
}

int SolverOutputSink::TeeBuf::sync() {
    if (!m_buf.empty()) {
        emitLine(QString::fromUtf8(m_buf.c_str(), static_cast<int>(m_buf.size())));
        m_buf.clear();
    }
    if (m_orig) m_orig->pubsync();
    return 0;
}

void SolverOutputSink::TeeBuf::emitLine(const QString& t) {
    if (!m_sink) return;
    QMetaObject::invokeMethod(m_sink, "pushText", Qt::QueuedConnection, Q_ARG(QString, t));
}
