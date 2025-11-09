#pragma once
#include "Source.h"
#include <vector>
#include <ostream>

namespace Rux
{
	enum class DiagnosticPhase
	{
		Unknown,
		Config,
		Lexer,
		Parser,
		Sematic,
		Generation
	};

	enum class DiagnosticSeverity
	{
		Error,
		Warning,
		Hint,
		Info
	};

	struct DiagnosticCode
	{
		DiagnosticPhase phase;
		DiagnosticSeverity severity;
		int code;
		const char* format;
	};

	struct Diagnostic
	{
		DiagnosticCode code;
		SourceLocation location;

	};

	class DiagnosticEngine
	{
	public:
		void Add(const Diagnostic& diagnostic);
		bool IsEmpty() const;
		void PrintAll(std::ostream& stream) const;
	private:
		std::vector<Diagnostic> diagnostics;
	};
}