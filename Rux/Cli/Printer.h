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
	};
}
