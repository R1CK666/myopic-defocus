# Myopic Defocus 1.3.5 — experimental prerelease

Independent Windows desktop chromatic filter. Not a medical device or a proven
myopia treatment; not an official Refractify release.

Changes: renamed product; persistent full/partial composite fix; capture-failure
recovery; texture checks; fresh-enable presentation state; wait-handle cleanup;
finite parameter parsing; single-instance protection; asynchronous GPU timers.

Known limitations: primary landscape display only; I-Beam flicker unresolved;
Windows/GPU runtime compatibility and long-term stability are not fully validated.
See BUG-CHECK.md and docs/参数调整说明.md. Preserve legacy settings automatically.

Close old instances before running. Ctrl+Alt+F8 toggles; if unresponsive, use
Ctrl+Alt+Delete → Task Manager to terminate the executable.
