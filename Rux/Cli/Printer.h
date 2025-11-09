#pragma once

namespace Rux::Cli
{
	class Printer
	{
	public:
		Printer();

		void PrintHelp();
		void PrintVersion();
		void PrintVersionDetailed();

	private:
		// Foreground colors
		static const char* Blue;
		static const char* Cyan;
		static const char* Gray;
		static const char* Green;
		static const char* Magenta;
		static const char* Red;
		static const char* Yellow;
		static const char* Reset;
	};
}
