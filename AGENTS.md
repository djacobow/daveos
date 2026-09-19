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
