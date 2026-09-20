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
* Other methods and interrupt callbacks submit requests or completion flags;
  they must not change current state directly.
* Apply this structure to all new state machines. Existing conversions are
  tracked separately in TODO.md rather than bundled into unrelated work.
