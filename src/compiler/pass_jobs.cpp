// The jobs pass (overview §2, Background jobs): the jobs that code/jobs/
// files define, by name, and code/schedule.ms, evaluated as data:
//
//   schedule = {}
//   schedule["cleanup"]   = "daily 03:00"
//   schedule["pollFeeds"] = "every 15m"
//
// A job's name is the key in its file's `job` map, and must be unique across
// code/jobs/. A schedule naming no job, or one that isn't a schedule, is an
// error.

#include "passes.h"

namespace fourfold {

void jobsPass(const CodeListing& listing, const CodeTree& code, const std::map<RelPath, PageHandlers>& handlers,
              DataEvaluator* evaluator, CompiledApp& app, Diagnostics& diagnostics) {
    const RelPath jobsFolder = RelPath::parse("jobs").value();
    for (const auto& [path, h] : handlers) {
        if (!path.isWithin(jobsFolder)) continue;
        if (h.job.empty()) {
            diagnostics.warning(codeFileName(path), "defines no jobs (job.name = function(args) ... end function)");
        }
        for (const std::string& name : h.job) {
            auto existing = app.jobs.find(name);
            if (existing != app.jobs.end()) {
                diagnostics.error(codeFileName(path), "job." + name + " is also defined in " +
                                                          codeFileName(existing->second.source) +
                                                          "; job names must be unique across code/jobs/");
                continue;
            }
            app.jobs.emplace(name, HandlerRef{HandlerRef::Kind::Job, path, name});
        }
    }

    const RelPath schedulePath = RelPath::parse("schedule.ms").value();
    const std::string fileName = codeFileName(schedulePath);
    bool hasFile = false;
    for (const CodeEntry& e : listing.files) hasFile = hasFile || e.path == schedulePath;
    if (!hasFile) return;
    auto file = code.read(schedulePath);
    if (!file) {
        diagnostics.error(fileName, file.error().message);
        return;
    }
    if (!evaluator) {
        diagnostics.error(fileName, "can't be read: no evaluator was provided");
        return;
    }
    EvaluatedData d = evaluator->evaluate(file.value(), "schedule");
    if (d.error) {
        diagnostics.error(d.error->file.empty() ? fileName : d.error->file, d.error->message, d.error->line);
        return;
    }
    if (d.value.kind() != Datum::Kind::Map) {
        diagnostics.error(fileName, d.value.isNull() ? "must set `schedule`, a map from job names to schedules (start "
                                                       "with schedule = {})"
                                                     : "`schedule` must be a map from job names to schedules");
        return;
    }
    for (const auto& [name, value] : d.value.asMap()) {
        std::string where = "schedule[\"" + name + "\"]: ";
        if (!app.jobs.count(name)) {
            diagnostics.error(fileName, where + "there's no job named " + name + " (job." + name + " in code/jobs/)");
            continue;
        }
        if (value.kind() != Datum::Kind::String) {
            diagnostics.error(fileName, where + "must be a string, like \"daily 03:00\" or \"every 15m\"");
            continue;
        }
        std::string error;
        std::optional<Schedule> s = parseSchedule(value.asString(), error);
        if (!s) {
            diagnostics.error(fileName, where + error);
            continue;
        }
        app.schedule.push_back({name, std::move(*s)});
    }
    if (d.value.asMap().empty()) diagnostics.warning(fileName, "schedules nothing");
}

}  // namespace fourfold
