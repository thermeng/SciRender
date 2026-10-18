#pragma once

#include <QObject>
#include <QString>
#include <streambuf>
#include <iostream>

class SolverOutputSink : public QObject {
    Q_OBJECT
public:
    explicit SolverOutputSink(QObject* parent = nullptr);
    ~SolverOutputSink() override;

    void install();
    void uninstall();

public slots:
    void pushText(const QString& t);

signals:
    void text(const QString& t);

private:
    class TeeBuf : public std::streambuf {
    public:
        TeeBuf(std::streambuf* orig, SolverOutputSink* sink);
    protected:
        int overflow(int c) override;
        std::streamsize xsputn(const char* s, std::streamsize n) override;
        int sync() override;
    private:
        void emitLine(const QString& t);
        std::streambuf* m_orig = nullptr;
        SolverOutputSink* m_sink = nullptr;
        std::string m_buf;
    };

    TeeBuf* m_coutBuf = nullptr;
    TeeBuf* m_cerrBuf = nullptr;
    std::streambuf* m_oldCout = nullptr;
    std::streambuf* m_oldCerr = nullptr;
};
