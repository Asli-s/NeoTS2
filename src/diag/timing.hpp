#pragma once

#include <cstdint>
#include <string>

// Time spent in named parts of the program, for measuring (neots2 --timings or --bench). A Timer adds
// the time from where it is made to the end of its scope to its name's total. While timings are off,
// a Timer only reads one flag.
namespace ts2::diag {

void setTimings(bool on);
bool timingsOn();

class Timer {
public:
    // `name` must live as long as the program (a text in quotes): timers are told apart by it.
    explicit Timer(const char* name);
    ~Timer();
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

private:
    const char* name_;
    std::int64_t start_ = -1;  // -1 while timings are off
};

// Each name's calls, average and longest time since the last call, one line each by name. It then
// starts counting again. Empty when nothing was timed.
std::string takeTimings();

} // namespace ts2::diag
