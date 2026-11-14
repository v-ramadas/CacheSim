// Definitions the unchanged Sniper sources need from Sniper's misc/log.h and misc/subsecond_time.cc:
// logging is off (zeroed Log object), errors abort; SubsecondTime prints as femtoseconds.
#include "../../../snipersim/common/misc/log.h"   // the real one (stubs/log.h is for the .cc files)
#include "subsecond_time.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

alignas(64) static unsigned char g_log_storage[sizeof(Log)] = {};
Log* Log::getSingleton() { return reinterpret_cast<Log*>(g_log_storage); }
String Log::getModule(const char*) { return String(""); }
void Log::log(ErrorState err, const char* file, SInt32 line, const char* format, ...) {
    if (err != Log::Error) return;
    va_list ap;
    va_start(ap, format);
    std::fprintf(stderr, "sniper error %s:%d: ", file, line);
    std::vfprintf(stderr, format, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
    std::abort();
}
std::ostream& operator<<(std::ostream& os, const SubsecondTime& t) { return os << t.getFS(); }
