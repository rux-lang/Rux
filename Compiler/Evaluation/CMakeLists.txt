# Sources and dependencies owned by Evaluation.

rux_add_component(RuxEvaluation
    TypedConstant.cpp)
target_link_libraries(RuxEvaluation PUBLIC RuxNumeric RuxTokens RuxTypes)
