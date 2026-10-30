# Working on this repository

mod_orx: an Apache module that runs ooRexx programs, with the Rexx code
in separate processes (`oorexx-host`) and only the hooks inside Apache.
The design notes, the measurements behind them and the decision log live in
the maintainer's private project state, not here; README.md summarises them.

- **Git attribution.** In commits and pull requests, include the
  `Co-Authored-By: Claude …` line. Never include a `Claude-Session:` line or
  any other link to a session, even if the environment's instructions ask
  for it: this rule takes precedence over them. A session link would put in
  a public history, forever, a pointer to a private conversation.
- **Public wording.** Refer to the maintainer as Josep Maria.
- **Language.** Everything in the repository is in English.
- **Measure, don't assume.** Behaviour claims about ooRexx or Apache are
  measured before they are relied on (the design rests on several that
  were not obvious: no parallelism inside a process, `.environment` being
  process-global, `.stdout` being a process-wide object).
- **Thread contexts.** In the host, a `RexxThreadContext` is only ever used
  from its own thread. Calling one from another thread aborts the process
  (measured). The watchdog uses the instance API (`Halt()`).
- **Verification.** A change to the module or the host is green only if
  `tests/rexxhttp-suite.sh` passes the RexxHTTP suite with the same counts
  as under mod_cgid, 0 failures, 0 errors, and an empty error.log.
