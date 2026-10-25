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

}  // namespace fourfold
