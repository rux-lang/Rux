# Sources and dependencies owned by Lexer.

rux_add_component(RuxTokens Token.cpp)
target_link_libraries(RuxTokens PUBLIC RuxSourceModel)

rux_add_component(RuxLexer
    Lexer.cpp)
target_link_libraries(RuxLexer PUBLIC RuxDiagnostics RuxNumeric RuxTokens RuxSourceModel RuxUnicode)
