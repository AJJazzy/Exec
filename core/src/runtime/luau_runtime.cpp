#include "nexus/runtime/luau_runtime.hpp"

#include "nexus/common/strings.hpp"
#include "nexus/compiler/compiler.hpp"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <unordered_map>

namespace nexus {

namespace {

using Clock = std::chrono::steady_clock;

enum class AbortReason { None, Cancelled, TimedOut, MemoryLimit };

struct PendingResume {
    Clock::time_point resumeAt;
    uint64_t order = 0;        // FIFO tie-breaker for equal resume times
    int nargs = 0;             // values already on the thread's stack for its first resume
    bool passElapsed = false;  // resuming from task.wait: pass the elapsed seconds
    Clock::time_point waitStart;
};

// Per-run state, reachable from C functions through lua_callbacks(L)->userdata and used as the
// allocator userdata. Everything except `cancelled` is only touched by the VM thread; `cancelled` is
// shared with the cancellation callback, which may fire after the run has ended.
struct Context {
    ExecutionLimits limits;
    std::string chunkName;
    const OutputCallback* output = nullptr;
    std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
    AbortReason abort = AbortReason::None;
    Clock::time_point start;
    Clock::time_point deadline;

    uint64_t memCurrent = 0;
    uint64_t memPeak = 0;

    uint64_t interrupts = 0;
    uint64_t outputBytes = 0;
    uint64_t droppedOutput = 0;
    bool outputLimitNotified = false;
    uint32_t threadsCreated = 0;
    uint32_t backgroundErrors = 0;

    lua_State* mainState = nullptr;
    lua_State* scriptThread = nullptr;
    bool mainFinished = false;
    std::optional<ScriptError> mainError;
    bool mainErrorIsMemory = false;

    std::unordered_map<lua_State*, int> refs;              // scheduler-owned threads -> registry ref
    std::unordered_map<lua_State*, PendingResume> pending; // threads waiting to be resumed
    uint64_t nextOrder = 0;

    double elapsedMs() const { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }

    void emit(OutputStream stream, std::string_view text) {
        if (!output || !*output) return;
        if (outputBytes + text.size() > limits.maxOutputBytes) {
            droppedOutput += text.size();
            if (!outputLimitNotified) {
                outputLimitNotified = true;
                (*output)(OutputEvent{OutputStream::System,
                                      "Output limit of " + str::formatBytes(limits.maxOutputBytes) +
                                          " reached; further output from this run is discarded.",
                                      elapsedMs()});
            }
            return;
        }
        outputBytes += text.size();
        (*output)(OutputEvent{stream, str::sanitizeUtf8(text), elapsedMs()});
    }

    const char* abortMessage() const {
        switch (abort) {
        case AbortReason::Cancelled: return "Execution cancelled";
        case AbortReason::TimedOut: return "Execution timed out";
        case AbortReason::MemoryLimit: return "Memory limit exceeded";
        case AbortReason::None: break;
        }
        return "Execution aborted";
    }
};

Context* contextOf(lua_State* L) { return static_cast<Context*>(lua_callbacks(L)->userdata); }

double threadCpuMs() {
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// ---------------------------------------------------------------------------------------------
// VM hooks

void* limitedAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    auto* ctx = static_cast<Context*>(ud);
    size_t oldSize = ptr ? osize : 0;
    if (nsize == 0) {
        std::free(ptr);
        ctx->memCurrent -= oldSize;
        return nullptr;
    }
    uint64_t next = ctx->memCurrent - oldSize + nsize;
    if (nsize > oldSize && next > ctx->limits.memoryLimitBytes) {
        // Refusing the allocation makes Luau raise a memory error. The run is also marked as
        // aborted so a pcall that catches the error cannot keep the script alive.
        if (ctx->abort == AbortReason::None) ctx->abort = AbortReason::MemoryLimit;
        return nullptr;
    }
    void* block = std::realloc(ptr, nsize);
    if (!block) return nullptr;
    ctx->memCurrent = next;
    ctx->memPeak = std::max(ctx->memPeak, next);
    return block;
}

void interruptHook(lua_State* L, int gc) {
    if (gc >= 0) return; // GC safepoint: raising errors here is not allowed
    Context* ctx = contextOf(L);
    if (!ctx) return;
    ++ctx->interrupts;
    if (ctx->abort == AbortReason::None) {
        if (ctx->cancelled->load(std::memory_order_relaxed)) ctx->abort = AbortReason::Cancelled;
        else if ((ctx->interrupts & 0x3FF) == 0 && Clock::now() >= ctx->deadline) ctx->abort = AbortReason::TimedOut;
    }
    if (ctx->abort != AbortReason::None) luaL_error(L, "%s", ctx->abortMessage());
}

// ---------------------------------------------------------------------------------------------
// Scheduler helpers

void adoptThread(Context* ctx, lua_State* L, lua_State* co) {
    if (ctx->refs.count(co)) return;
    lua_pushthread(co);
    lua_xmove(co, L, 1);
    ctx->refs[co] = lua_ref(L, -1);
    lua_pop(L, 1);
}

void releaseThread(Context* ctx, lua_State* co) {
    auto it = ctx->refs.find(co);
    if (it != ctx->refs.end()) {
        lua_unref(ctx->mainState, it->second);
        ctx->refs.erase(it);
    }
    ctx->pending.erase(co);
}

void schedule(Context* ctx, lua_State* co, Clock::time_point at, int nargs, bool passElapsed) {
    PendingResume p;
    p.resumeAt = at;
    p.order = ctx->nextOrder++;
    p.nargs = nargs;
    p.passElapsed = passElapsed;
    p.waitStart = Clock::now();
    ctx->pending[co] = p;
}

ScriptError makeScriptError(Context* ctx, lua_State* co) {
    ScriptError err;
    if (lua_isstring(co, -1)) {
        size_t len = 0;
        const char* s = lua_tolstring(co, -1, &len);
        err.raw.assign(s, len);
    } else {
        err.raw = std::string("(error object is a ") + luaL_typename(co, -1) + " value)";
    }
    err.raw = str::sanitizeUtf8(err.raw);
    auto [line, message] = splitErrorLocation(err.raw, ctx->chunkName);
    err.line = line;
    err.message = message;
    if (const char* trace = lua_debugtrace(co)) err.traceback = str::sanitizeUtf8(trace);
    err.frames = parseTraceback(err.traceback);
    if (err.line == 0) {
        for (const auto& f : err.frames) {
            if (f.source == ctx->chunkName && f.line) {
                err.line = f.line;
                break;
            }
        }
    }
    return err;
}

// Resumes `co` and handles the outcome. `from` is the currently running thread for nested resumes
// (task.spawn) or nullptr when called from the scheduler loop.
void resumeThread(Context* ctx, lua_State* co, lua_State* from, int nargs) {
    int status = lua_resume(co, from, nargs);
    if (status == LUA_YIELD) {
        if (!ctx->pending.count(co)) {
            // Yielded without asking the scheduler to resume it (e.g. a bare coroutine.yield()).
            if (co == ctx->scriptThread) {
                ctx->emit(OutputStream::System, "The main script yielded without scheduling a resume; it will not continue.");
                ctx->mainFinished = true;
            }
            releaseThread(ctx, co);
        }
        return;
    }
    if (status == LUA_OK) {
        if (co == ctx->scriptThread) ctx->mainFinished = true;
        releaseThread(ctx, co);
        return;
    }

    // Error.
    if (ctx->abort == AbortReason::None || co == ctx->scriptThread) {
        ScriptError err = makeScriptError(ctx, co);
        if (co == ctx->scriptThread) {
            ctx->mainError = err;
            ctx->mainErrorIsMemory = status == LUA_ERRMEM;
            ctx->mainFinished = true;
        } else if (ctx->abort == AbortReason::None) {
            ++ctx->backgroundErrors;
            std::string text = err.raw;
            if (!err.traceback.empty()) text += "\nStack trace:\n" + err.traceback;
            ctx->emit(OutputStream::Error, text);
        }
    }
    releaseThread(ctx, co);
}

// Prepares a thread to run `f` (or adopts an existing suspended thread) with the arguments at
// stack indices [firstArg, top]. Leaves the thread object on L's stack and returns it; *nargsOut
// receives the number of arguments to pass on its next resume.
lua_State* prepareTaskThread(lua_State* L, int fnIndex, int firstArg, int* nargsOut) {
    Context* ctx = contextOf(L);
    int top = lua_gettop(L);
    int nargs = std::max(0, top - firstArg + 1);
    *nargsOut = nargs;

    if (lua_isthread(L, fnIndex)) {
        lua_State* co = lua_tothread(L, fnIndex);
        int st = lua_costatus(L, co);
        if (st != LUA_COSUS) luaL_error(L, "cannot resume a %s thread", (st == LUA_COFIN || st == LUA_COERR) ? "dead" : "running");
        for (int i = firstArg; i <= top; ++i) lua_pushvalue(L, i);
        lua_xmove(L, co, nargs);
        adoptThread(ctx, L, co);
        ctx->pending.erase(co);
        lua_pushvalue(L, fnIndex);
        return co;
    }

    luaL_checktype(L, fnIndex, LUA_TFUNCTION);
    if (ctx->refs.size() >= ctx->limits.maxThreads)
        luaL_error(L, "task limit exceeded (%u live threads)", static_cast<unsigned>(ctx->limits.maxThreads));
    lua_State* co = lua_newthread(L); // pushed on L
    lua_pushvalue(L, fnIndex);
    for (int i = firstArg; i <= top; ++i) lua_pushvalue(L, i);
    lua_xmove(L, co, nargs + 1);
    adoptThread(ctx, L, co);
    ++ctx->threadsCreated;
    return co;
}

// ---------------------------------------------------------------------------------------------
// Library functions

int nx_print(lua_State* L, OutputStream stream) {
    int n = lua_gettop(L);
    std::string line;
    for (int i = 1; i <= n; ++i) {
        size_t len = 0;
        const char* s = luaL_tolstring(L, i, &len);
        if (i > 1) line += '\t';
        line.append(s, len);
        lua_pop(L, 1);
    }
    if (Context* ctx = contextOf(L)) ctx->emit(stream, line);
    return 0;
}

int nx_printOutput(lua_State* L) { return nx_print(L, OutputStream::Output); }
int nx_warn(lua_State* L) { return nx_print(L, OutputStream::Warning); }

int nx_loadstring(lua_State* L) {
    size_t len = 0;
    const char* src = luaL_checklstring(L, 1, &len);
    std::string chunk = luaL_optstring(L, 2, "loadstring");
    CompileOutput out;
    try {
        out = compileSource(std::string_view(src, len));
    } catch (const std::exception& e) {
        luaL_error(L, "loadstring: %s", e.what());
    }
    if (!out.ok) {
        lua_pushnil(L);
        std::string msg = out.diagnostics.empty() ? "compilation failed" : formatDiagnostic(out.diagnostics.front(), chunk);
        lua_pushlstring(L, msg.data(), msg.size());
        return 2;
    }
    std::string name = "=" + chunk;
    if (luau_load(L, name.c_str(), out.bytecode.data(), out.bytecode.size(), 0) != 0) {
        lua_pushnil(L);
        lua_insert(L, -2);
        return 2;
    }
    return 1;
}

int task_wait(lua_State* L) {
    Context* ctx = contextOf(L);
    double seconds = luaL_optnumber(L, 1, 0);
    if (!std::isfinite(seconds) || seconds < ctx->limits.minimumWaitSeconds) seconds = ctx->limits.minimumWaitSeconds;
    if (!lua_isyieldable(L)) luaL_error(L, "task.wait cannot yield here (called across a C-call or metamethod boundary)");
    adoptThread(ctx, L, L);
    auto delay = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
    schedule(ctx, L, Clock::now() + delay, 0, true);
    return lua_yield(L, 0);
}

int task_spawn(lua_State* L) {
    Context* ctx = contextOf(L);
    int nargs = 0;
    lua_State* co = prepareTaskThread(L, 1, 2, &nargs);
    int threadIndex = lua_gettop(L);
    resumeThread(ctx, co, L, nargs);
    lua_pushvalue(L, threadIndex);
    return 1;
}

int task_defer(lua_State* L) {
    Context* ctx = contextOf(L);
    int nargs = 0;
    lua_State* co = prepareTaskThread(L, 1, 2, &nargs);
    schedule(ctx, co, Clock::now(), nargs, false);
    return 1;
}

int task_delay(lua_State* L) {
    Context* ctx = contextOf(L);
    double seconds = luaL_checknumber(L, 1);
    if (!std::isfinite(seconds) || seconds < 0) seconds = 0;
    int nargs = 0;
    lua_State* co = prepareTaskThread(L, 2, 3, &nargs);
    auto delay = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
    schedule(ctx, co, Clock::now() + delay, nargs, false);
    return 1;
}

int task_cancel(lua_State* L) {
    Context* ctx = contextOf(L);
    luaL_checktype(L, 1, LUA_TTHREAD);
    lua_State* co = lua_tothread(L, 1);
    if (co == L) luaL_error(L, "cannot cancel the running thread");
    int st = lua_costatus(L, co);
    if (st == LUA_CONOR) luaL_error(L, "cannot cancel a thread that is resuming another thread");
    releaseThread(ctx, co);
    if (st == LUA_COSUS) lua_resetthread(co);
    return 0;
}

void registerNexusLibraries(lua_State* L, const RuntimeOptions& options) {
    lua_pushcfunction(L, nx_printOutput, "print");
    lua_setglobal(L, "print");
    lua_pushcfunction(L, nx_warn, "warn");
    lua_setglobal(L, "warn");
    if (options.enableLoadstring) {
        lua_pushcfunction(L, nx_loadstring, "loadstring");
        lua_setglobal(L, "loadstring");
    }
    if (options.enableTaskLibrary) {
        static const luaL_Reg taskLib[] = {
            {"wait", task_wait}, {"spawn", task_spawn}, {"defer", task_defer},
            {"delay", task_delay}, {"cancel", task_cancel}, {nullptr, nullptr},
        };
        luaL_register(L, "task", taskLib);
        lua_pop(L, 1);
    }
}

struct StateCloser {
    void operator()(lua_State* L) const {
        lua_callbacks(L)->interrupt = nullptr;
        lua_close(L);
    }
};

std::string describeLimitMs(std::chrono::milliseconds ms) { return str::formatDuration(static_cast<double>(ms.count())); }

} // namespace

ExecutionResult LuauRuntime::run(std::string_view source, const RuntimeOptions& options, const OutputCallback& output,
                                 const CancellationToken& cancel) {
    ExecutionResult result;
    result.target = options.targetId;

    auto ctx = std::make_unique<Context>();
    ctx->limits = options.limits;
    ctx->chunkName = options.chunkName.empty() ? "Script" : options.chunkName;
    ctx->output = &output;
    ctx->start = Clock::now();
    ctx->deadline = ctx->start + options.limits.timeout;
    double cpuStart = threadCpuMs();

    Context* raw = ctx.get();
    cancel.onCancel([flag = ctx->cancelled] { flag->store(true, std::memory_order_relaxed); });

    CompileOutput compiled = compileSource(source);
    result.stats.compileMs = compiled.parseMilliseconds + compiled.compileMilliseconds;
    if (!compiled.ok) {
        result.status = ExecutionStatus::CompileError;
        result.diagnostics = compiled.diagnostics;
        if (!compiled.diagnostics.empty()) {
            const Diagnostic& d = compiled.diagnostics.front();
            result.error = ScriptError{d.message, formatDiagnostic(d, ctx->chunkName), d.range.line, {}, {}};
        }
        result.summary = "Compilation failed with " + std::to_string(compiled.diagnostics.size()) + " error(s)";
        result.stats.wallMs = ctx->elapsedMs();
        ctx->output = nullptr;
        return result;
    }
    result.stats.bytecodeBytes = compiled.bytecode.size();

    {
        std::unique_ptr<lua_State, StateCloser> state(lua_newstate(limitedAlloc, raw));
        if (!state) {
            result.status = ExecutionStatus::InternalError;
            result.summary = "Failed to create the Luau VM (memory limit too small?)";
            return result;
        }
        lua_State* L = state.get();
        ctx->mainState = L;
        lua_callbacks(L)->userdata = raw;

        luaL_openlibs(L);
        registerNexusLibraries(L, options);
        luaL_sandbox(L);

        lua_State* T = lua_newthread(L);
        ctx->refs[T] = lua_ref(L, -1);
        lua_pop(L, 1);
        luaL_sandboxthread(T);
        // luaL_sandbox freezes the original _G. As in Roblox, _G is a writable table shared by the
        // run's threads but distinct from the script's own globals.
        lua_newtable(T);
        lua_setglobal(T, "_G");
        ctx->scriptThread = T;

        std::string chunk = "=" + ctx->chunkName;
        if (luau_load(T, chunk.c_str(), compiled.bytecode.data(), compiled.bytecode.size(), 0) != 0) {
            result.status = ExecutionStatus::InternalError;
            result.summary = std::string("Failed to load bytecode: ") + (lua_tostring(T, -1) ? lua_tostring(T, -1) : "?");
            return result;
        }

        lua_callbacks(L)->interrupt = interruptHook;

        resumeThread(raw, T, nullptr, 0);

        // Scheduler loop: resume due threads in (time, FIFO) order and sleep until the next timer.
        while (ctx->abort == AbortReason::None && !ctx->mainError && !ctx->pending.empty()) {
            auto now = Clock::now();
            if (ctx->cancelled->load()) {
                ctx->abort = AbortReason::Cancelled;
                break;
            }
            if (now >= ctx->deadline) {
                ctx->abort = AbortReason::TimedOut;
                break;
            }
            std::vector<std::pair<lua_State*, PendingResume>> due;
            Clock::time_point next = Clock::time_point::max();
            for (const auto& [co, p] : ctx->pending) {
                if (p.resumeAt <= now) due.emplace_back(co, p);
                else next = std::min(next, p.resumeAt);
            }
            if (due.empty()) {
                if (cancel.waitUntil(std::min(next, ctx->deadline))) ctx->cancelled->store(true);
                continue;
            }
            std::sort(due.begin(), due.end(), [](const auto& a, const auto& b) {
                return a.second.resumeAt != b.second.resumeAt ? a.second.resumeAt < b.second.resumeAt : a.second.order < b.second.order;
            });
            for (const auto& [co, p] : due) {
                if (ctx->abort != AbortReason::None || ctx->mainError) break;
                auto it = ctx->pending.find(co);
                if (it == ctx->pending.end() || it->second.order != p.order) continue; // cancelled or rescheduled
                ctx->pending.erase(it);
                int nargs = p.nargs;
                if (p.passElapsed) {
                    lua_pushnumber(co, std::chrono::duration<double>(Clock::now() - p.waitStart).count());
                    nargs = 1;
                }
                resumeThread(raw, co, nullptr, nargs);
            }
        }
        // Threads still pending when the run ends are discarded together with the VM.
    }

    result.stats.wallMs = ctx->elapsedMs();
    result.stats.cpuMs = threadCpuMs() - cpuStart;
    result.stats.peakMemoryBytes = ctx->memPeak;
    result.stats.interruptChecks = ctx->interrupts;
    result.stats.outputBytes = ctx->outputBytes;
    result.stats.droppedOutputBytes = ctx->droppedOutput;
    result.stats.threadsCreated = ctx->threadsCreated;
    result.stats.backgroundErrors = ctx->backgroundErrors;

    switch (ctx->abort) {
    case AbortReason::Cancelled:
        result.status = ExecutionStatus::Cancelled;
        result.summary = "Stopped by user after " + str::formatDuration(result.stats.wallMs);
        break;
    case AbortReason::TimedOut:
        result.status = ExecutionStatus::TimedOut;
        result.summary = "Stopped: exceeded the " + describeLimitMs(options.limits.timeout) + " time limit";
        break;
    case AbortReason::MemoryLimit:
        result.status = ExecutionStatus::MemoryLimit;
        result.summary = "Stopped: exceeded the " + str::formatBytes(options.limits.memoryLimitBytes) + " memory limit";
        break;
    case AbortReason::None:
        if (ctx->mainError) {
            result.status = ctx->mainErrorIsMemory ? ExecutionStatus::MemoryLimit : ExecutionStatus::RuntimeError;
            result.error = ctx->mainError;
            result.summary = (ctx->mainError->line ? "Runtime error on line " + std::to_string(ctx->mainError->line) + ": "
                                                   : std::string("Runtime error: ")) +
                             ctx->mainError->message;
            Diagnostic d;
            d.severity = Severity::Error;
            d.code = "runtime";
            d.message = ctx->mainError->message;
            d.range.line = d.range.endLine = ctx->mainError->line;
            d.origin = "runtime";
            result.diagnostics.push_back(std::move(d));
        } else {
            result.status = ExecutionStatus::Succeeded;
            result.summary = "Completed in " + str::formatDuration(result.stats.wallMs);
            if (ctx->backgroundErrors)
                result.summary += " (" + std::to_string(ctx->backgroundErrors) + " error(s) in spawned tasks)";
        }
        break;
    }
    if (ctx->abort != AbortReason::None && ctx->mainError) result.error = ctx->mainError;
    ctx->output = nullptr;
    return result;
}

// ---------------------------------------------------------------------------------------------

namespace {

struct StandaloneInventory {
    std::vector<std::string> globals;
    std::map<std::string, std::vector<std::string>> members;
};

const StandaloneInventory& inventory() {
    static const StandaloneInventory inv = [] {
        StandaloneInventory out;
        std::unique_ptr<lua_State, StateCloser> state(luaL_newstate());
        lua_State* L = state.get();
        luaL_openlibs(L);
        registerNexusLibraries(L, RuntimeOptions{});
        lua_pushnil(L);
        while (lua_next(L, LUA_GLOBALSINDEX) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                std::string name = lua_tostring(L, -2);
                out.globals.push_back(name);
                if (lua_istable(L, -1) && name != "_G") {
                    std::vector<std::string> members;
                    lua_pushnil(L);
                    while (lua_next(L, -2) != 0) {
                        if (lua_type(L, -2) == LUA_TSTRING) members.emplace_back(lua_tostring(L, -2));
                        lua_pop(L, 1);
                    }
                    std::sort(members.begin(), members.end());
                    out.members[name] = std::move(members);
                }
            }
            lua_pop(L, 1);
        }
        std::sort(out.globals.begin(), out.globals.end());
        return out;
    }();
    return inv;
}

} // namespace

const std::vector<std::string>& standaloneGlobals() { return inventory().globals; }

bool isStandaloneGlobal(std::string_view name) {
    const auto& g = inventory().globals;
    return std::binary_search(g.begin(), g.end(), name, [](const auto& a, const auto& b) { return std::string_view(a) < std::string_view(b); });
}

const std::map<std::string, std::vector<std::string>>& standaloneLibraryMembers() { return inventory().members; }

} // namespace nexus
