#include "Diagnostic.h"

void Rux::DiagnosticEngine::Add(const Diagnostic& diagnostic)
{
	diagnostics.push_back(diagnostic);
}

bool Rux::DiagnosticEngine::IsEmpty() const
{
	return diagnostics.size() == 0;
}

void Rux::DiagnosticEngine::PrintAll(std::ostream& stream) const
{
	for (const auto& diag : diagnostics)
	{
		stream << diag.location.filename << ":" << diag.location.line << ":" << diag.location.column << " ";
		switch (diag.code.severity)
		{
		case Rux::DiagnosticSeverity::Error:
			stream << "error ";
			break;
		case Rux::DiagnosticSeverity::Warning:
			stream << "warning ";
			break;
		case Rux::DiagnosticSeverity::Hint:
			stream << "hint ";
			break;
		case Rux::DiagnosticSeverity::Info:
			stream << "info ";
			break;
		}
		stream << diag.code.code << ": ";
		// For simplicity, we won't implement format string processing here
		stream << diag.code.format << std::endl;
	}
}
