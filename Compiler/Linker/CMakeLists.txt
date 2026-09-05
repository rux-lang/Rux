# Sources and dependencies owned by Linker.

rux_add_component(RuxArchive
    ArchiveWriter.cpp)
target_link_libraries(RuxArchive PUBLIC RuxObjectRcu)

rux_add_component(RuxLinker
    AArch64Relocation.cpp
    Coff/CoffObjectWriter.cpp
    Elf/ElfObjectWriter.cpp
    Linker.cpp
    RcuLinkGraph.cpp
    RcuObjectLayout.cpp
    MachO/CodeSignature.cpp
    MachO/MachOLayout.cpp
    MachO/MachOLinkEdit.cpp
    MachO/MachOObjectWriter.cpp
    NativeObjectWriter.cpp
    Pe/PeWriter.cpp
    Elf/ElfWriter.cpp
    MachO/MachOWriter.cpp)
target_link_libraries(RuxLinker PUBLIC RuxObjectRcu RuxSystem RuxArchive RuxCrypto)
