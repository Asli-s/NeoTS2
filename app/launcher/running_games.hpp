#pragma once

namespace ts2::launcher {

// True while an original Sims 2 game runs on this computer. The original games run as Sims2.exe,
// Sims2EP<n>.exe or Sims2SP<n>.exe. Which save folder it uses can't be seen from outside, so any
// running game counts. Always false off Windows.
bool originalGameRunning();

// True when another launcher is already open. The answer is kept until this launcher closes.
bool otherLauncherOpen();

} // namespace ts2::launcher
