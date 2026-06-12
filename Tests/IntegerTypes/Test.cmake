include("${CMAKE_CURRENT_LIST_DIR}/../RuxTestingFramework.cmake")

message(STATUS "Using rux: ${RUX}")

rux_register_error(1 "int8 minimum != -128")
rux_register_error(2 "int8 maximum != 127")
rux_register_error(3 "Underflow fails for int8")
rux_register_error(4 "Overflow fails for int8")
rux_register_error(5 "~0i8 != -1")
rux_register_error(6 "~5i8 != -6")
rux_register_error(7 "(-i8 as uint8) != 255u8")
rux_register_error(8 "(255u8 as int8) != -1i8")
rux_register_error(9 "(1i8 << 7) != -128")
rux_register_error(10 "(-128i8 >> 1) != -64")

rux_register_error(11 "uint8 minimum != 0")
rux_register_error(12 "uint8 maximum != 255")
rux_register_error(13 "Underflow fails for uint8")
rux_register_error(14 "Overflow fails for uint8")
rux_register_error(15 "~0u8 != 255u8")
rux_register_error(16 "~255u8 != 0u8")
rux_register_error(17 "(1u8 << 7) != 128u8")
rux_register_error(18 "(255u8 >> 1) != 127u8")

rux_register_error(19 "int16 minimum != -32768")
rux_register_error(20 "int16 maximum != 32767")
rux_register_error(21 "Underflow fails for int16")
rux_register_error(22 "Overflow fails for int16")
rux_register_error(23 "~0i16 != -1")
rux_register_error(24 "~5i16 != -6")
rux_register_error(25 "(-1i16 as uint16) != 65535u16")
rux_register_error(26 "(65535u16 as int16) != -1i16")
rux_register_error(27 "(1i16 << 15) != -32768")
rux_register_error(28 "(-32768i16 >> 1) != -16384")

rux_register_error(29 "Underflow fails for uint16")
rux_register_error(30 "Overflow fails for uint16")
rux_register_error(31 "~0u16 != 65535u16")
rux_register_error(32 "~65535u16 != 0u16")
rux_register_error(33 "(1u16 << 15) != 32768u16")
rux_register_error(34 "(65535u16 >> 1) != 32767u16")

rux_register_error(35 "Underflow fails for int32")
rux_register_error(36 "Overflow fails for int32")
rux_register_error(37 "~0i32 != -1")
rux_register_error(38 "(-1i32 as uint32) != 4294967295u32")
rux_register_error(39 "(4294967295u32 as int32) != -1i32")
rux_register_error(40 "(1i32 << 31) != -2147483648")
rux_register_error(41 "(-2147483648i32 >> 1) != -1073741824")

rux_register_error(42 "Underflow fails for uint32")
rux_register_error(43 "Overflow fails for uint32")
rux_register_error(44 "~0u32 != 4294967295u32")
rux_register_error(45 "~4294967295u32 != 0u32")
rux_register_error(46 "(1u32 << 31) != 2147483648u32")
rux_register_error(47 "(4294967295u32 >> 1) != 2147483647u32")

rux_register_error(48 "Underflow fails for int64")
rux_register_error(49 "Overflow fails for int64")
rux_register_error(50 "~0i64 != -1")
rux_register_error(51 "(-1i64 as uint64) != 18446744073709551615u64")
rux_register_error(52 "(18446744073709551615u64 as int64) != -1i64")
rux_register_error(53 "(1i64 << 63) != -9223372036854775808")
rux_register_error(54 "(-9223372036854775808i64 >> 1) != -4611686018427387904")

rux_register_error(55 "Underflow fails for uint64")
rux_register_error(56 "Overflow fails for uint64")
rux_register_error(57 "~0u64 != 18446744073709551615u64")
rux_register_error(58 "~18446744073709551615u64 != 0u64")
rux_register_error(59 "(1u64 << 63) != 9223372036854775808u64")
rux_register_error(60 "(18446744073709551615u64 >> 1) != 9223372036854775807u64")

rux_register_error(61 "5 + 7 != 12")
rux_register_error(62 "5 - 7 != -2")
rux_register_error(63 "-5 * 7 != -35")
rux_register_error(64 "-35 / 7 != -5")
rux_register_error(65 "-35 % 7 != 0")

rux_register_error(66 "-7 / 2 != -3")
rux_register_error(67 "7 / -2 != -3")
rux_register_error(68 "-7 / -2 != 3")
rux_register_error(69 "-7 % 2 != -1")
rux_register_error(70 "7 % -2 != 1")

rux_register_error(71 "Equality operator failed")
rux_register_error(72 "Inequality operator failed")
rux_register_error(73 "Less-than operator failed")
rux_register_error(74 "Less-than-or-equal operator failed")
rux_register_error(75 "Greater-than operator failed")
rux_register_error(76 "Greater-than-or-equal operator failed")

rux_register_error(77 "Compound += failed")
rux_register_error(78 "Compound -= failed")
rux_register_error(79 "Compound *= failed")
rux_register_error(80 "Compound /= failed")
rux_register_error(81 "Compound %= failed")

rux_register_error(82 "Bit mask low nibble extraction failed")
rux_register_error(83 "Bit mask high nibble extraction failed")

rux_register_error(84 "<< by zero failed")
rux_register_error(85 "128u8 >> 7 != 1")
rux_register_error(86 "Arithmetic right shift failed")

rux_register_error(87 "int8 -> int16 sign extension failed")
rux_register_error(88 "int8 -> int32 sign extension failed")
rux_register_error(89 "int8 -> int64 sign extension failed")

rux_register_error(90 "300i32 as int8 != 44")
rux_register_error(91 "0xDEADBEEF as uint8 != 0xEF")

rux_register_error(92 "Constant folding failed for A")
rux_register_error(93 "Constant folding failed for B")

rux_register_error(94 "Operator precedence failed: multiplication")
rux_register_error(95 "Operator precedence failed: parentheses")
rux_register_error(96 "Operator precedence failed: shift/addition")

rux_register_error(97 "Unary minus failed")
rux_register_error(98 "Bitwise complement failed")

rux_register_error(99 "Bitwise OR failed")
rux_register_error(100 "Bitwise XOR failed")

rux_register_error(101 "Compound &= failed")
rux_register_error(102 "Compound |= failed")
rux_register_error(103 "Compound ^= failed")

rux_register_error(104 "Compound <<= failed")
rux_register_error(105 "Compound >>= failed")

rux_register_error(106 "20 << 2 != 80")
rux_register_error(107 "20 >> 2 != 5")
rux_register_error(108 "-8 >> 2 != -2")

rux_register_error(109 "5 << 3 != 40")
rux_register_error(110 "5 >> 2 != 1")

rux_register_error(111 "Hex literal parsing failed")
rux_register_error(112 "Octal literal parsing failed")
rux_register_error(113 "Binary literal parsing failed")

rux_register_error(114 "uint8 -> uint16 zero extension failed")
rux_register_error(115 "uint8 -> uint32 zero extension failed")
rux_register_error(116 "uint8 -> uint64 zero extension failed")

rux_register_error(117 "int8 -> uint32 conversion failed")

rux_register_error(118 "Left associativity of subtraction failed")
rux_register_error(119 "Double bitwise complement identity failed")

rux_register_error(120 "XOR identity x ^ x == 0 failed")
rux_register_error(121 "AND identity x & all_ones == x failed")
rux_register_error(122 "OR identity x | 0 == x failed")

rux_prepare_test("${TEST_DIR}" "integer_types_test" target_binary)

rux_assert_executable("${target_binary}")