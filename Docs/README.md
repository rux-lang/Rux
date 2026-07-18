# Rux Contributor Docs

Process and architecture documentation for people working **on** the Rux compiler. If you are looking for how to _use_ the language, see the [website docs](https://rux-lang.dev) instead.

Start with [CONTRIBUTING.md](../CONTRIBUTING.md), then use the guide that matches the work you are doing:

| Guide                                    | Use it when you need to                                                           |
| ---------------------------------------- | --------------------------------------------------------------------------------- |
| [Development Workflow](Workflow.md)      | Configure a development build, run tests, format code, or find a component        |
| [Compiler Architecture](Architecture.md) | Understand component ownership, dependency direction, or the compilation pipeline |
| [Branch Architecture](Branches.md)       | Create a topic branch or understand how changes reach a release                   |
| [Pull Request Lifecycle](PullRequest.md) | Prepare, review, update, or merge a pull request                                  |
| [CI/CD Flow](CI-CD.md)                   | Reproduce a required check or understand platform-specific CI                     |
| [Release Pipeline](Release.md)           | Prepare a version, tag it, verify artifacts, and publish a release                |

Installer-specific maintenance instructions live beside their implementation:

- [Linux installer](../Packaging/Linux/README.md)
- [Windows installers](../Packaging/Windows/README.md)

> These are living documents. When you change a process, branch rule, or workflow, update the matching page in the **same** pull request.

Commands in these pages are shown for POSIX shells unless a PowerShell example is provided. On Windows, replace `./Bin/Release/rux` with `.\Bin\Release\rux.exe`.
