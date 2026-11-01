set(MF_MIN_VERSION "1.0.0")
set(MF_NEEDS_LILA OFF)

file(GLOB_RECURSE _mf_follow_sources CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/community/mf_follow/src/*.cpp"
)

add_library(mf_follow OBJECT
    ${_mf_follow_sources}
)

target_include_directories(mf_follow PUBLIC
    "${CMAKE_SOURCE_DIR}/community/mf_follow/src"
)

set_target_properties(mf_follow PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

target_link_libraries(mf_follow PUBLIC MayaFlux::MayaFluxLib)

if(MF_NEEDS_LILA)
    target_link_libraries(mf_follow PUBLIC MayaFlux::MayaFluxHost)
endif()
