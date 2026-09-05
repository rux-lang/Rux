# Sources and dependencies owned by Syntax.

rux_add_component(RuxAst
    DocumentationComment.cpp
    DocumentationTags.cpp)
target_link_libraries(RuxAst PUBLIC RuxTokens RuxTarget)

rux_add_component(RuxSyntax
    Parser/Parser.cpp
    Parser/ParserAsm.cpp
    Parser/ParserDecl.cpp
    Parser/ParserAttributes.cpp
    Parser/ParserDeclDispatch.cpp
    Parser/ParserType.cpp
    Parser/ParserStmt.cpp
    Parser/ParserExpr.cpp
    Parser/ParserDump.cpp
    Parser/ParserDumpDecl.cpp
    Parser/ParserDumpExpr.cpp
    Parser/ParserDumpStmt.cpp)
target_link_libraries(RuxSyntax PUBLIC RuxAst RuxLexer RuxDiagnostics RuxTarget)
