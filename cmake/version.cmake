# Writes ${VERSION_HEADER} (version.h) with the version from `git describe`: 1.2.0 on a v1.2.0 tag,
# 1.2.0-3-gabc1234 three commits after it, -dirty with uncommitted changes, the short commit hash
# before the first tag, "unknown" outside a git checkout. The file changes only with the version.
execute_process(
    COMMAND git describe --tags --match "v[0-9]*" --always --dirty
    WORKING_DIRECTORY "${VERSION_SOURCE_DIR}"
    OUTPUT_VARIABLE version
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE failed)
if(failed OR version STREQUAL "")
    set(version "unknown")
endif()
string(REGEX REPLACE "^v" "" version "${version}")
file(CONFIGURE OUTPUT "${VERSION_HEADER}" CONTENT "#pragma once\n\n#define MUOS_SENDSPIN_VERSION \"@version@\"\n" @ONLY)
