# Sources and dependencies owned by Object.

rux_add_component(RuxObjectRcu
    Rcu/RcuDump.cpp
    Rcu/RcuWriter.cpp)
target_link_libraries(RuxObjectRcu PUBLIC RuxBuildInfo RuxTarget)
