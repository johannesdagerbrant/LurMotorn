// Android implementations of the shared app-startup verbs (issue #43, Phase 3 section B).
#include "Lur/App/Platform.h"

#include <android/log.h>

#include "Lur/Core/Log.h"
#include "Lur/Core/LogTag.h"

namespace Lur::App::Platform {
namespace {

void AndroidLogSink(bool Error, const char* Line, void* /*User*/) {
    __android_log_print(Error ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO,
                        Lur::Core::LogTag, "%s", Line);
}

} // namespace

void InstallLogSink() { Lur::Log::Init(&AndroidLogSink, Lur::Core::LogTag); }

} // namespace Lur::App::Platform
