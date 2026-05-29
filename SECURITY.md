# Security Policy

## Supported Versions

AuraIO is pre-1.0 software. Security fixes are made against the latest
released version and the `main` branch.

| Version | Supported          |
| ------- | ------------------ |
| 0.7.x   | :white_check_mark: |
| < 0.7   | :x:                |

## Reporting a Vulnerability

AuraIO issues raw `io_uring` submissions and manages registered file
descriptors and pinned buffers on behalf of the caller, so memory-safety and
privilege issues are taken seriously.

**Please do not report security vulnerabilities through public GitHub
issues.**

Instead, report them privately using one of:

- **GitHub Security Advisories** (preferred): open a private report via the
  ["Report a vulnerability"](https://github.com/Par-B/AuraIO/security/advisories/new)
  button on the repository's Security tab.
- **Email**: par.botes@gmail.com

Please include:

- A description of the vulnerability and its impact.
- Steps to reproduce (a minimal program, kernel version, and liburing version
  are especially helpful).
- Any suggested mitigation, if known.

## What to Expect

- **Acknowledgement** within 5 business days.
- An initial assessment and severity classification shortly after.
- Coordinated disclosure: we will work with you on a fix and a disclosure
  timeline, and credit you in the release notes unless you prefer to remain
  anonymous.

Thank you for helping keep AuraIO and its users safe.
