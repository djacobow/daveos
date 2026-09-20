# AGENTS.md -- development guide for AI coding agents

## Project Overview

A lightweight cooperative scheduler for embedded targets written in c++20,
with a build system in meson and tools written in Python

## Before Pushing 

* update affected docs and comments in the code
* Always run all the tests, formatting, and lint and fix issues

## Commit messages

* Include a short validation summary in the commit body.
* Distinguish host tests, ARM builds, programming-plan checks, and actual hardware
  testing; state material hardware validation gaps. Do not rewrite published
  commits just to add validation summaries.

## State machines

* Represent state with an `enum class`.
* Begin each tick with a next-state variable initialized from current state
  (`State ns = cs`).
* Only the `switch (cs)` may change `ns`.
* After the switch, commit `cs = ns` only if the two differ. This must be the
  single runtime assignment to current state; declaration-time initialization
  is allowed.
* With the CRTP `core::StateMachine` helper, the base begins the tick with
  `ns = cs` and performs the single commit after the derived `Step(cs, ns, ...)`
  returns. The derived switch remains the only place that changes `ns`;
  entry/exit/tick hooks perform actions, not transitions.
* Declare state enums and machine classes at the narrowest practical scope.
* Other methods and interrupt callbacks submit requests or completion flags;
  they must not change current state directly.
* Use the shared helper for new state machines. Per-tick context and input may
  be passed as borrowed arguments; do not retain them beyond the tick.
* Status snapshots (such as link state) and persistent image eligibility are
  data, not tick-driven machines; do not add artificial transitions to them.

## Real OTP provisioning

* The connected DUT may already contain OTP data and locks. Start with a
  read-only scan; preserve unknown, damaged and provisioned blocks.
* Report the scan and proposed block before any irreversible write or lock.
* Once initial write/lock validation succeeds, automated hardware tests must
  only read real OTP. Further real writes require explicit user instruction.
* Use FileOtp or the bank-B emulator for repeated writes, failure injection,
  exhaustion, and destructive tests. Factory main-flash erase is not OTP erase.
