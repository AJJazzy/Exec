#pragma once
// Wire protocol between Nexus and nexus-runner: newline-delimited JSON over the runner's stdin and
// stdout. One runner process executes exactly one request and then exits.
//
// host -> runner   {"type":"run","protocol":1,"source":"...","chunkName":"Script","limits":{...}}
//                  {"type":"cancel"}
// runner -> host   {"type":"ready","protocol":1,"pid":123,"sandbox":{...},"limits":[...]}
//                  {"type":"output","event":{"stream":"output","text":"...","elapsedMs":1.2}}
//                  {"type":"result","result":{...ExecutionResult...}}

#include "nexus/execution/target.hpp"

#include <string>
#include <string_view>

namespace nexus::runner {

constexpr int kProtocolVersion = 1;
constexpr size_t kMaxMessageBytes = 64ull << 20; // a single protocol line may not exceed this

json::Value makeRunMessage(const ExecutionRequest& request);
json::Value makeCancelMessage();
json::Value makeOutputMessage(const OutputEvent& event);
json::Value makeResultMessage(const ExecutionResult& result);

// Accumulates bytes and yields complete lines.
class LineBuffer {
public:
    void append(std::string_view bytes) { buffer_.append(bytes); }
    // Extracts the next complete line (without the newline). Returns false if none is available.
    bool next(std::string& line);
    size_t pendingBytes() const { return buffer_.size() - offset_; }

private:
    std::string buffer_;
    size_t offset_ = 0;
};

} // namespace nexus::runner
