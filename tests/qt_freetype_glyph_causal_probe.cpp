// Diagnostic wrapper: the unchanged fixed behavior probe executes once. Warning
// stacks are captured after the original FT failure and never repair rendering.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <execinfo.h>
#include <mutex>
#include <cxxabi.h>

#define main qcae_fixed_font_probe_main
#include "qt_font_backend_behavior_probe.cpp"
#undef main

namespace {
struct WarningTrace {
    QString message;
    std::array<void*, 48> frames{};
    int frame_count{};
};
std::mutex trace_mutex;
std::vector<WarningTrace> warning_traces;
QtMessageHandler original_handler{};
thread_local bool tracing_warning{};
void traceWarning(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    if (!tracing_warning && message.startsWith(QLatin1String("load glyph failed err="))) {
        tracing_warning = true;
        WarningTrace trace;
        trace.message = message;
        trace.frame_count = backtrace(trace.frames.data(), static_cast<int>(trace.frames.size()));
        {
            const std::lock_guard guard(trace_mutex);
            warning_traces.push_back(std::move(trace));
        }
        tracing_warning = false;
    }
    if (original_handler)
        original_handler(type, context, message);
    else {
        const auto formatted = qFormatLogMessage(type, context, message).toLocal8Bit();
        std::fwrite(formatted.constData(), 1, static_cast<std::size_t>(formatted.size()), stderr);
        std::fputc('\n', stderr);
    }
}
QJsonArray capturedStacks() {
    const std::lock_guard guard(trace_mutex);
    QJsonArray traces;
    for (const auto& trace : warning_traces) {
        QJsonArray frames;
        for (int i = 0; i < trace.frame_count; ++i) {
            Dl_info info{};
            const auto address = reinterpret_cast<std::uintptr_t>(trace.frames[i]);
            QJsonObject frame{{"index", i}, {"address", QString::number(address, 16)}};
            if (dladdr(trace.frames[i], &info)) {
                const auto image = QString::fromLocal8Bit(info.dli_fname ? info.dli_fname : "");
                frame.insert("image", image);
                frame.insert("image_sha256", fileHash(image));
                frame.insert("image_offset",
                             QString::number(
                                 address - reinterpret_cast<std::uintptr_t>(info.dli_fbase), 16));
                if (info.dli_sname) {
                    frame.insert("symbol", QString::fromLatin1(info.dli_sname));
                    int status{};
                    auto* name = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
                    if (status == 0 && name)
                        frame.insert("demangled", QString::fromLatin1(name));
                    std::free(name);
                    frame.insert(
                        "symbol_offset",
                        QString::number(address - reinterpret_cast<std::uintptr_t>(info.dli_saddr),
                                        16));
                }
            }
            frames.append(frame);
        }
        traces.append(QJsonObject{{"original_warning", trace.message}, {"frames", frames}});
    }
    return traces;
}
} // namespace

int main(int argc, char** argv) {
    original_handler = qInstallMessageHandler(traceWarning);
    const auto result = qcae_fixed_font_probe_main(argc, argv);
    qInstallMessageHandler(original_handler);
    const auto path = qEnvironmentVariable("QCAE_FT_WARNING_REPORT");
    if (path.isEmpty()) {
        std::fputs("QCAE_FT_WARNING_REPORT must preserve the causal trace\n", stderr);
        return 2;
    }
    try {
        writeReport(path,
                    {{"schema", "qcae.ft-glyph-warning-stack/1"},
                     {"fixed_probe_exit_code", result},
                     {"original_probe_called_once", true},
                     {"warning_forwarded_once", true},
                     {"library_or_product_modified", false},
                     {"whole_copy_coverage", "unknown"},
                     {"warning_traces", capturedStacks()}});
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 2;
    }
    return result;
}
