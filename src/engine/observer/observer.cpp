#include "observer/observer.h"

namespace fourfold {

void ObserverList::compileStarted() {
    for (Observer* o : observers_) o->compileStarted();
}

void ObserverList::compileFinished(const CompileReport& report) {
    for (Observer* o : observers_) o->compileFinished(report);
}

void ObserverList::scriptError(const ScriptError& error) {
    for (Observer* o : observers_) o->scriptError(error);
}

void ObserverList::renderWarnings(uint64_t generation, const std::vector<RenderWarning>& warnings) {
    for (Observer* o : observers_) o->renderWarnings(generation, warnings);
}

void ObserverList::limitsCounted(const std::string& request,
                                 const std::vector<std::pair<std::string, double>>& counts) {
    for (Observer* o : observers_) o->limitsCounted(request, counts);
}

void ObserverList::outboundCall(const OutboundCall& call) {
    for (Observer* o : observers_) o->outboundCall(call);
}

void ObserverList::jobsEnqueued(const std::string& source, const std::vector<std::string>& names, bool committed) {
    for (Observer* o : observers_) o->jobsEnqueued(source, names, committed);
}

}  // namespace fourfold
