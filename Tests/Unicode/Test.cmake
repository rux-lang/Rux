include("${CMAKE_CURRENT_LIST_DIR}/../RuxTestingFramework.cmake")

message(STATUS "Using rux: ${RUX}")

# 1-byte ASCII decoding (\u{41} -> "A")
rux_register_error(1  "ASCII escape: String length is not 1 byte")
rux_register_error(2  "ASCII escape: Byte 0 mismatch (Expected 0x41)")

# 2-byte UTF-8 decoding (U+00E9 -> 0xC3 0xA9)
rux_register_error(3  "2-byte UTF-8 escape: String length is not 2 bytes")
rux_register_error(4  "2-byte UTF-8 escape: Byte 0 mismatch (Expected 0xC3)")
rux_register_error(5  "2-byte UTF-8 escape: Byte 1 mismatch (Expected 0xA9)")

# 3-byte UTF-8 decoding (U+4E2D -> 0xE4 0xB8 0xAD)
rux_register_error(6  "3-byte UTF-8 escape: String length is not 3 bytes")
rux_register_error(7  "3-byte UTF-8 escape: Byte 0 mismatch (Expected 0xE4)")
rux_register_error(8  "3-byte UTF-8 escape: Byte 1 mismatch (Expected 0xB8)")
rux_register_error(9  "3-byte UTF-8 escape: Byte 2 mismatch (Expected 0xAD)")

# 4-byte UTF-8 astral plane decoding (U+1F600 -> 0xF0 0x9F 0x98 0x80)
rux_register_error(10 "4-byte UTF-8 escape: String length is not 4 bytes")
rux_register_error(11 "4-byte UTF-8 escape: Byte 0 mismatch (Expected 0xF0)")
rux_register_error(12 "4-byte UTF-8 escape: Byte 1 mismatch (Expected 0x9F)")
rux_register_error(13 "4-byte UTF-8 escape: Byte 2 mismatch (Expected 0x98)")
rux_register_error(14 "4-byte UTF-8 escape: Byte 3 mismatch (Expected 0x80)")

# Character literal raw value decoding
rux_register_error(15 "Char literal escape: Code point value mismatch (Expected decimal 128512)")

# Classic escape sequences fallback verification (\t\n\\)
rux_register_error(16 "Classic escape sequences: Combined string length is not 3 bytes")
rux_register_error(17 "Classic escape sequences: Tab character (\\t) byte mismatch (Expected 0x09)")
rux_register_error(18 "Classic escape sequences: Newline character (\\n) byte mismatch (Expected 0x0A)")
rux_register_error(19 "Classic escape sequences: Backslash character (\\\\) byte mismatch (Expected 0x5C)")

rux_prepare_test("${TEST_DIR}" "unicode_test" target_binary)

rux_assert_executable("${target_binary}")