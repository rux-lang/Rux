# Sources and dependencies owned by Types.

rux_add_component(RuxTypes
    Type.cpp
    PrimitiveCatalog.cpp
    TypeProperties.cpp
    NativeConversion.cpp)
target_link_libraries(RuxTypes PUBLIC RuxTarget)
