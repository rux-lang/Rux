#pragma once

#include <string>

namespace Rux
{
	// Location information in source code
	struct SourceLocation
	{
		// Source file name
		std::string filename;

		// Line number in the file
		size_t  line;

		// Column in the line
		size_t column;

		// Byte offset in the file
		size_t  index;   

		SourceLocation(std::string_view filename, size_t line, size_t column, size_t index)
			: filename(filename), line(line), column(column), index(index) { }
	};
}