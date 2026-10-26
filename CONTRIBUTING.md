# Contributing to NeoTS2

Thanks for helping. A few rules keep the project healthy and legally clean.

## What never goes in the repository

- **Game files or anything extracted from them**: packages, textures, models, sounds,
  saves, or exports made from them.
- **Code taken from the game**, or from other projects whose license doesn't allow it.
- **Personal or local details**: paths on your machine, logs, traces.

## While NeoTS2 is early

The engine is still taking shape, so for now the most helpful things are bug reports and small, tested fixes.
Please open an issue before starting anything big (new features, renderers or ports), so we can check it fits first.

## How pull requests are merged

NeoTS2 is developed in a private repository, and this one gets a snapshot of it every few days.
So a pull request can't be merged here directly. Once it's reviewed and ready, I bring it into the
private repository, and it comes out in the next snapshot. You're the author of that commit, and
credited in the README. Then I close the pull request with a link to it.

If you build on top of your own open pull request, expect to rebase onto the snapshot once it's out.

## Code

- Follow the surrounding code: C++20, the existing naming and formatting.
- Game files are untrusted input. Check every size, count and index read from them before using it.
- Behaviour should match the original game. Say in the pull request how you checked it.
- Comments say what and why, in plain sentences.
- Windows players should see no change from work aimed at other systems, unless that's the point
  of the change.
