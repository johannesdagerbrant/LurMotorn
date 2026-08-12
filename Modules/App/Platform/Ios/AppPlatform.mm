// iOS implementations of the shared app-startup verbs (issue #43, Phase 3 section B).
//
// Objective-C++ because os_log is the platform log; the header stays plain C++ so a game's own .cpp
// can include it without dragging Apple headers in.
#include "Lur/App/Platform.h"

#include <os/log.h>

#include <fcntl.h>    // O_NONBLOCK on stdout/stderr — see UnblockStdio
#include <unistd.h>
#include <cstdlib>    // setenv: MoltenVK log level, before any vkCreateInstance

#include "Lur/Core/Log.h"
#include "Lur/Core/LogTag.h"

namespace Lur::App::Platform {
namespace {

// %{public}s is MANDATORY — a plain %s is redacted to <private> unless Xcode is attached, which is
// never our case for a sideloaded dev build (see the iOS notes in CLAUDE.md, and #197's redaction
// postmortem: the diagnostic that exists to show device ids was hiding them).
void IosLogSink(bool Error, const char* Line, void* /*User*/) {
    if (Error) os_log_error(OS_LOG_DEFAULT, "%{public}s: %{public}s", Lur::Core::LogTag, Line);
    else       os_log(OS_LOG_DEFAULT, "%{public}s: %{public}s", Lur::Core::LogTag, Line);
}

} // namespace

void InstallLogSink() { Lur::Log::Init(&IosLogSink, Lur::Core::LogTag); }

void UnblockStdio() {
    setenv("MVK_CONFIG_LOG_LEVEL", "1", /*overwrite*/ 0);  // 1 = errors only; 0 respects an override
    for (int Fd : {STDOUT_FILENO, STDERR_FILENO}) {
        const int Flags = fcntl(Fd, F_GETFL, 0);
        if (Flags != -1) fcntl(Fd, F_SETFL, Flags | O_NONBLOCK);
    }
}

} // namespace Lur::App::Platform
