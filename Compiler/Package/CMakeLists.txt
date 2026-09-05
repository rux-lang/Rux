# Sources and dependencies owned by Package.

rux_add_component(RuxPackage
    Artifact.cpp
    Checksum.cpp
    Identity.cpp
    Manifest.cpp
    ManifestSerialization.cpp
    ManifestSyntax.cpp
    ManifestValidation.cpp
    Package.cpp
    PublicationValidation.cpp
    Version.cpp)
target_link_libraries(RuxPackage PUBLIC RuxCrypto RuxTarget)

rux_add_component(RuxPackageCache
    Cache.cpp)
target_link_libraries(RuxPackageCache PUBLIC RuxPackage RuxSystem)

rux_add_component(RuxPackageServices
    Credentials.cpp
    Installation.cpp
    PackageResolution.cpp
    Registry.cpp)
target_link_libraries(RuxPackageServices PUBLIC RuxPackageCache PRIVATE RuxBuildInfo RuxHttp)
