#pragma once

#include <string>

namespace sitcom {

void LogInit(const std::wstring& log_path, bool enabled);
// Always attempts to append a line if a log file was opened (bootstrap or config).
void LogWrite(const std::string& line);
bool LogEnabled();
// Force-open a log next to the DLL even before config is read.
bool LogBootstrap(const std::wstring& preferred_path);
// NDJSON debug ingest (session bb2a06). Safe to call from game/worker threads.
void DebugNdjson(const char* hypothesis_id, const char* location, const char* message,
                 const char* data_json);

}  // namespace sitcom
