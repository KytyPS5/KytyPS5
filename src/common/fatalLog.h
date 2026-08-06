#ifndef KYTY_COMMON_FATAL_LOG_H_
#define KYTY_COMMON_FATAL_LOG_H_

namespace Common {

// Optional mirror path (e.g. --printf-output-file). Always appends to _kyty_fatal.txt as well.
void SetFatalLogMirrorPath(const char* path);

void LogFatalToFile(const char* message);

// Lock-free-ish last-chance log next to the exe (_kyty_crash_last.txt). Safe from VEH.
void EmergencyLogRaw(const char* message);

// Overwrites _kyty_heartbeat.txt next to the exe (last known alive point).
void HeartbeatLog(const char* message);

} // namespace Common

#endif /* KYTY_COMMON_FATAL_LOG_H_ */
