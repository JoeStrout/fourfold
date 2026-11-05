// DevTools: what dev mode adds to the browser (overview §9; Arch §5
// devtools). M3 has live reload and the error overlay:
//
// - Every HTML page gets a small script, /__dev/client.js, which listens to
//   /__dev/reload (server-sent events). After a successful recompile the
//   page reloads, or, if only stylesheets changed, swaps them in place.
// - After a failed compile, open pages show an overlay with each error's
//   file, line, message, and the code around it, while the site keeps
//   serving the last good version. Fixing the error reloads them.
// - A script error while handling a request gets a 500 page in the same
//   style, instead of plain text.
// - Template warnings (a field with no value, a value with no field) show
//   in a small panel on open pages, once per app generation.
//
// - /__dev/access (M6): the access report, every URL the app serves with its
//   effective access level and the rules that produced it, and the access
//   warnings; /__dev/access.json has the same as data. From M7, also each
//   route's rate limits, timing check, challenge and form.
// - /__dev/limits (M7): the rate-limit counters, the latest requests counted
//   (and refused), and a button that resets the counters.
// - /__dev/migrations (M8): which migrations are applied, pending, failed or
//   changed, with "redo last" and "reset dev data".
//
// It's an Observer (compile events come in) and the engine's EngineHooks
// (requests for /__dev/ and HTML responses go through it). The core never
// depends on it; production doesn't create one.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "base/datum.h"
#include "observer/observer.h"
#include "partition/code_tree.h"
#include "pipeline/hooks.h"
#include "pipeline/stages.h"
#include "platform/event_loop.h"

namespace fourfold {

class DevTools : public Observer, public EngineHooks {
public:
    // `loop` is the transport's loop, where the event streams live; it must
    // outlive this object. `code` is read for the code around errors.
    DevTools(EventLoop& loop, CodeTree code);

    // Where the access report finds the app serving now. Set before serving.
    void setAppSource(std::function<std::shared_ptr<const CompiledApp>()> current) { currentApp_ = std::move(current); }

    // Where /__dev/limits reads the counters. Set before serving; may be null.
    void setRateStore(RateStore* rates) { rates_ = rates; }

    // What /__dev/migrations shows and does (overview §3, Dev mode), over
    // the host's app instance. Each call waits.
    struct MigrationPanel {
        // {"items": [{number, name, file, state, appliedAt}], "lastError",
        // "problems": [...], "waiting"}
        std::function<Datum()> status;
        // Each returns what happened, for the developer.
        std::function<std::string()> redoLast;
        std::function<std::string()> resetData;
    };
    void setMigrationPanel(MigrationPanel panel) { migrations_ = std::move(panel); }

    // The access report's data: [{url, kind, access, sources, limits,
    // form}], and warnings.
    static Datum accessReport(const CompiledApp& app);
    ~DevTools() override;

    DevTools(const DevTools&) = delete;
    DevTools& operator=(const DevTools&) = delete;

    // Observer
    void compileFinished(const CompileReport& report) override;
    void renderWarnings(uint64_t generation, const std::vector<RenderWarning>& warnings) override;
    void scriptError(const ScriptError& error) override;
    void limitsCounted(const std::string& request, const std::vector<std::pair<std::string, double>>& counts) override;

    // EngineHooks
    std::optional<HttpResponse> reserved(const HttpRequest& request, const AdmittedRequest& admitted) override;
    void decorate(HttpResponse& response, uint64_t generation, const std::string& nonce) override;
    HttpResponse scriptErrorPage(const ScriptError& error) override;

    // Open /__dev/reload streams. On the loop's thread.
    size_t streamCount() const;

    struct Hub;   // private

private:
    // Errors as the overlay shows them: file, line, message, and the code
    // around the line.
    Datum describeErrors(const std::vector<Diagnostic>& diagnostics) const;

    CodeTree code_;
    std::shared_ptr<Hub> hub_;
    std::function<std::shared_ptr<const CompiledApp>()> currentApp_;   // shared with tasks posted to the loop
    RateStore* rates_ = nullptr;
    MigrationPanel migrations_;
    std::mutex messageMutex_;
    std::string migrationMessage_;   // the last action's outcome, shown once
};

// The client script served at /__dev/client.js.
extern const char* const kDevClientScript;

// One server-sent event: "event: <name>\ndata: <json>\n\n".
std::string serverSentEvent(const std::string& name, const Datum& data);

// Inserts `tag` before the last </body> (any case), or at the end.
void insertBeforeBodyEnd(std::string& html, const std::string& tag);

}  // namespace fourfold
