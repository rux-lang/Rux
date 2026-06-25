# Security Policy

The Rux team takes the security of the compiler and its toolchain seriously. Thank you for helping keep Rux and its users safe.

## Supported Versions

Rux is under active development. Security fixes are applied to the latest released minor version only. We recommend always running the most recent release.

## Reporting a Vulnerability

**Please do not report security vulnerabilities through public GitHub issues, pull requests, or discussions.**

Instead, report them privately using one of the following channels:

- **Email:** [info@rux-lang.dev](mailto:info@rux-lang.dev)
- **GitHub:**
  Use [private vulnerability reporting](https://docs.github.com/en/code-security/how-tos/report-and-fix-vulnerabilities/report-privately)
  via the repository's **Security** tab → **Report a vulnerability**.

To help us triage quickly, please include as much of the following as you can:

- The type of issue (e.g. memory corruption, code injection via crafted source, path traversal in the package manager, etc.)
- The affected component (lexer, parser, codegen, linker, CLI, package manager)
- Rux version and host platform (OS, architecture, compiler)
- Step-by-step instructions to reproduce, including any minimal source input
- Proof-of-concept or exploit code, if available
- The impact of the issue and how an attacker might exploit it

Reports written in English are preferred.

## Our Commitment

When you report a vulnerability, you can expect us to:

- **Acknowledge** receipt of your report within **3 business days**.
- Provide an initial **assessment** within **10 business days**.
- Keep you informed of progress toward a fix and a release.
- Treat your report **confidentially** and not share your details without your permission.
- **Credit** you for the discovery when the fix is published, unless you prefer to remain anonymous.

## Disclosure Policy

We follow a **coordinated disclosure** process:

1. You report the vulnerability privately.
2. We confirm the issue and develop a fix.
3. We prepare a release and a security advisory.
4. We publicly disclose the vulnerability after a fix is available, typically within **90 days** of the initial report.

We ask that you give us a reasonable opportunity to address the issue before any public disclosure.

## Safe Harbor

We consider security research conducted in good faith and in accordance with this policy to be authorized. We will not pursue or support legal action against researchers who:

- Make a good-faith effort to avoid privacy violations, data destruction, and service disruption.
- Only interact with systems and accounts they own or have explicit permission to access.
- Report vulnerabilities promptly and do not exploit them beyond what is necessary to demonstrate the issue.

Thank you for helping make Rux more secure.
