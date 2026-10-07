# Security and privacy

The application does not intentionally send desktop images, keystrokes, or settings
to a network service. It captures the primary desktop locally to render a filter.
Diagnostics record selected operation names, timestamps and HRESULT errors, not
screenshots or input text. This is a source-level design description, not an
independent security audit.

Never post passwords, tokens, private screenshots or sensitive logs in issues.
For an overlay/input issue, disable the filter or terminate it via Task Manager.
Report reproducible non-sensitive bugs in GitHub Issues. Experimental status and
medical limitations are documented in README and BUG-CHECK.md.
