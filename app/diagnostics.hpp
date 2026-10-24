#pragma once

namespace app {

// Starts the log and crash reports, before anything else runs, and takes their options out of the
// arguments (so the rest never sees them):
//   --log-folder <folder>  where logs and crash reports go, by default "logs" beside the program
//                          when it can write there (a portable copy), else the user's app data folder
//   --no-crash-dialog      write crash reports without asking anything (for tests)
//   --crash-test <kind>    crash on purpose, to check reports (see crash::test)
//   --send-report <folder> only send that saved crash report, then end (see report_upload.hpp)
void startDiagnostics(int& argc, char** argv);

// The build's version, e.g. "0.1.0-dev.42+8c5f11b976" (see docs/code-guide.md).
const char* buildVersion();
// Whether this is a release build (configured with NEOTS2_RELEASE=ON).
bool releaseBuild();

} // namespace app
