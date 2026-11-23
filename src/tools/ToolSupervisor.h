// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// Supervisor of an external preparation tool (rar, 7z, par2), plan_queue.md
// lot C5.3. ngPost runs it as itself, in a hidden mode dispatched by main()
// before any Qt object exists:
//
//   ngPost --ngpost-tool-supervisor <proofFile> <nonce> -- <tool> <args...>
//
// It adopts every orphaned descendant of the tool (PR_SET_CHILD_SUBREAPER),
// runs the tool only once told to ("go"), and writes its proof file only once
// no descendant is left (waitpid() answering ECHILD): from then on, nothing
// it started can still write. Linux only; elsewhere it refuses to run.
//
// Channels: fd 3, lines to the controller (ready, started, exec_failed,
// tool_exited, finished, proof_failed); fd 4, lines from it (go, stop), whose
// end of file means the controller is gone: the tool's tree is stopped.
//
//========================================================================

#ifndef TOOLS_TOOLSUPERVISOR_H
#define TOOLS_TOOLSUPERVISOR_H

namespace toolsupervisor
{

//! The hidden command line switch.
constexpr const char *kSwitch = "--ngpost-tool-supervisor";
//! Exit codes of the supervisor itself (the tool's status is in the proof).
constexpr int kProven = 0;
constexpr int kSetupFailed = 2;
constexpr int kProofFailed = 3;

//! Runs the supervisor for argv = { self, kSwitch, proof, nonce, "--", tool, args... }.
int run(int argc, char **argv);

} // namespace toolsupervisor

#endif // TOOLS_TOOLSUPERVISOR_H
