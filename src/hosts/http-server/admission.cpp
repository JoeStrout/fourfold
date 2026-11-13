#include <deque>

#include "httpserver/http_server.h"

namespace fourfold {

namespace {

struct Admission : std::enable_shared_from_this<Admission> {
    struct Waiting {
        HttpRequest request;
        Responder respond;
        EventLoop::TimerId timer = 0;
        uint64_t id = 0;
    };

    EventLoop& loop;
    RequestHandler handler;
    AdmissionLimits limits;
    size_t running = 0;
    uint64_t nextId = 1;
    std::deque<Waiting> waiting;

    Admission(EventLoop& l, RequestHandler h, AdmissionLimits lim) : loop(l), handler(std::move(h)), limits(lim) {}

    static HttpResponse busy() {
        HttpResponse r = HttpResponse::text(503, "Service Unavailable: the server is busy; try again shortly\n");
        r.headers.set("Retry-After", "5");
        return r;
    }

    void take(HttpRequest request, Responder respond) {
        if (running < limits.maxRunning) {
            start(std::move(request), std::move(respond));
            return;
        }
        if (waiting.size() >= limits.maxWaiting) {
            respond(busy());
            return;
        }
        Waiting w{std::move(request), std::move(respond), 0, nextId++};
        std::weak_ptr<Admission> weak = shared_from_this();
        uint64_t id = w.id;
        w.timer = loop.addTimer(limits.maxWait, [weak, id] {
            auto self = weak.lock();
            if (!self) return;
            for (auto it = self->waiting.begin(); it != self->waiting.end(); ++it) {
                if (it->id != id) continue;
                Responder r = std::move(it->respond);
                self->waiting.erase(it);
                r(busy());
                return;
            }
        });
        waiting.push_back(std::move(w));
    }

    void start(HttpRequest request, Responder respond) {
        running++;
        std::weak_ptr<Admission> weak = shared_from_this();
        auto answered = std::make_shared<bool>(false);
        handler(std::move(request), [weak, respond, answered](HttpResponse response) {
            if (*answered) return;
            *answered = true;
            respond(std::move(response));
            if (auto self = weak.lock()) self->finished();
        });
    }

    void finished() {
        running--;
        while (running < limits.maxRunning && !waiting.empty()) {
            Waiting w = std::move(waiting.front());
            waiting.pop_front();
            loop.cancelTimer(w.timer);
            start(std::move(w.request), std::move(w.respond));
        }
    }
};

}  // namespace

RequestHandler limitConcurrency(EventLoop& loop, RequestHandler handler, AdmissionLimits limits) {
    auto admission = std::make_shared<Admission>(loop, std::move(handler), limits);
    return [admission](HttpRequest request, Responder respond) {
        admission->take(std::move(request), std::move(respond));
    };
}

}  // namespace fourfold
