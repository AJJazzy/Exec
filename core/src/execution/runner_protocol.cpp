#include "nexus/execution/runner_protocol.hpp"

namespace nexus::runner {

json::Value makeRunMessage(const ExecutionRequest& request) {
    return json::Object{
        {"type", "run"},
        {"protocol", kProtocolVersion},
        {"source", request.source},
        {"chunkName", request.scriptName},
        {"limits", request.limits.toJson()},
    };
}

json::Value makeCancelMessage() { return json::Object{{"type", "cancel"}}; }

json::Value makeOutputMessage(const OutputEvent& event) { return json::Object{{"type", "output"}, {"event", event.toJson()}}; }

json::Value makeResultMessage(const ExecutionResult& result) { return json::Object{{"type", "result"}, {"result", result.toJson()}}; }

bool LineBuffer::next(std::string& line) {
    size_t nl = buffer_.find('\n', offset_);
    if (nl == std::string::npos) {
        // Compact consumed data so the buffer does not grow without bound.
        if (offset_ > 0) {
            buffer_.erase(0, offset_);
            offset_ = 0;
        }
        return false;
    }
    line.assign(buffer_, offset_, nl - offset_);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    offset_ = nl + 1;
    if (offset_ == buffer_.size()) {
        buffer_.clear();
        offset_ = 0;
    }
    return true;
}

} // namespace nexus::runner
