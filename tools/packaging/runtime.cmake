# Invoked after cmake --install, never against a shared system prefix.
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()
if(WIN32)
    set(excludes POST_EXCLUDE_REGEXES "[Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\]([Ss][Yy][Ss][Tt][Ee][Mm]32|[Ss][Yy][Ss][Ww][Oo][Ww]64)[/\\\\].*")
    set(pre PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*")
elseif(APPLE)
    set(excludes POST_EXCLUDE_REGEXES "^/usr/lib/.*" "^/System/Library/.*")
else()
    # The Linux archive targets the build distribution's libc/C++ ABI, like other
    # native CLI packages. These remain platform prerequisites, not bundled files.
    set(pre PRE_EXCLUDE_REGEXES "^ld-linux.*" "^lib(c|m|dl|pthread|rt|resolv|z|stdc\\+\\+|gcc_s)\\.so.*"
        "^lib(va|va-drm|drm)\\.so(\\..*)?$")
endif()
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${executable}"
    DIRECTORIES ${search_directories}
    RESOLVED_DEPENDENCIES_VAR resolved
    UNRESOLVED_DEPENDENCIES_VAR unresolved
    ${pre} ${excludes})
if(unresolved)
    message(FATAL_ERROR "Unresolved runtime dependencies: ${unresolved}")
endif()
list(JOIN resolved "\n" result)
file(WRITE "${output}" "${result}\n")
