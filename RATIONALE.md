# DaveOS -- why?

I wrote DaveOS because I've made similar schedulers before for various
projects at companies I worked for, and wanted a framework of my own that
I could keep adding to and using for projects. I also wanted to work in
C++ for strong type safety and the ability to do more generic programming
through templates rather than elaborate macros.

This is an opinionated framework for the kinds of embedded applications
I like to write. Simplicity, concision, and avoiding duplication matter
more to me than providing every possible scheduling model.

## Is it LLM-generated?

Yes, DaveOS was mostly written by OpenAI's Astra. I wrote a detailed
specification based on my experience, then instructed the tool to
interview me about unsettled details. We iterated until the specification
was concrete enough to implement, then continued refining it as we built
and tested the system.

The specification, implementation, and tests all need review. Agreement
between me and an LLM isn't evidence that the code works; host tests,
compiler checks, and experiments on real hardware provide that evidence.
Other LLM reviews have also helped identify gaps, but don't replace those
checks. The [TODO list](TODO.md) records both completed validation and
things we haven't qualified yet.

## Why not use FreeRTOS?

FreeRTOS works fine, so this is a reasonable question. Basically, I have
a preference for cooperative callbacks and explicit state machines in my
projects. Blocking calls in a preemptive OS are convenient, but in my
experience they don't remove the need to think carefully about shared
peripherals, buffer ownership, timeouts, and interactions between tasks.
I prefer to make that state explicit from the beginning.

FreeRTOS can also use [cooperative scheduling](https://github.com/FreeRTOS/FreeRTOS-Kernel/blob/main/examples/template_configuration/FreeRTOSConfig.h).
The distinction here is more specific: a DaveOS task is a callback, not
an independently suspended thread with its own stack. Long operations
normally advance through a state machine over successive invocations.
Between deliberate yield points, one scheduled callback cannot be
interrupted by another scheduled callback.

That makes some reasoning easier, but it doesn't eliminate concurrency.
Hardware interrupts and DMA still operate independently. Shared data and
borrowed buffers still need appropriate protection and lifetime rules.

There are costs, too. A callback that takes too long delays other work,
and synchronous libraries need careful adaptation. DaveOS provides a
limited nested `yield()` for such cases, but it isn't a general thread
scheduler. A nested task must return before its caller can resume, and
must never wait for a resource held by that suspended caller. A design
that needs priority-based task preemption may be better served by an RTOS.

But a fair part of the answer is still that I like this way of working.

## One shared stack

DaveOS callbacks share the scheduler's call stack, so I don't have to
reserve a separate stack for each task. That can make RAM budgeting less
wasteful when different tasks need substantial temporary space at
different times.

It doesn't make stack sizing disappear. The budget still has to cover
the deepest call chain, interrupt frames, and any nested yielded tasks.
A yielding caller's stack remains occupied while another task runs.
Stack painting helps measure exercised workloads; it doesn't prove a
worst-case bound. The [memory notes](docs/memory.md) describe how we check
this on the STM32 targets.

## Why C++20?

I want types to express the contracts: which module owns a callback,
which payload an event carries, and which arguments a command accepts.
Templates and compile-time checks let us catch many mistakes before the
firmware reaches the board. Small registration macros are still useful,
but the underlying behavior belongs in ordinary C++.

Using C++ doesn't mean requiring exceptions or a runtime heap. The core
uses fixed storage and application-owned objects. Standard library types
are useful when they fit those constraints. Optional networking uses
fixed preallocated pools; host test infrastructure can allocate freely.

## More than a scheduler, with optional pieces

A scheduler alone leaves a lot of repetitive application work. Logging,
commands, bus access, storage, and health monitoring are things I want to
reuse, too. They should remain separable: an application can have logging
without commands, commands without logging, or neither.

Hardware dependencies are supplied explicitly so the same logic can run
against an STM32 peripheral or a host implementation. Fake time makes
scheduling tests fast and repeatable; file-backed flash and OTP models
let us exercise failures without wearing out or permanently programming
a board. Those models complement hardware tests rather than reproducing
every electrical, timing, or power-loss behavior.

The aim is to keep application behavior understandable while giving it
useful building blocks. The [specification](PROJECT.md) defines the
contracts; this document explains why I chose them.
