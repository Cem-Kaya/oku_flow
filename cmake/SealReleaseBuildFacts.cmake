# Run only after the application target's successful link (or the bundle's
# controlled signing step). Metadata readers never create this build seal.
if(NOT DEFINED FactsPath OR NOT EXISTS "${FactsPath}")
    message(FATAL_ERROR "Release build seal requires generated target facts")
endif()
file(READ "${FactsPath}" facts)
string(JSON application GET "${facts}" application)
if(NOT EXISTS "${application}" OR IS_DIRECTORY "${application}")
    message(FATAL_ERROR "Release build seal requires the successfully built application")
endif()
file(SHA256 "${FactsPath}" facts_sha256)
file(SHA256 "${application}" application_sha256)
string(REGEX REPLACE "\\.json$" ".seal.json" seal_path "${FactsPath}")
if(seal_path STREQUAL FactsPath)
    message(FATAL_ERROR "Release build facts must have a JSON filename")
endif()
file(WRITE "${seal_path}.tmp"
    "{\n  \"schema\": 1,\n  \"factsSha256\": \"${facts_sha256}\",\n  \"applicationSha256\": \"${application_sha256}\"\n}\n")
file(RENAME "${seal_path}.tmp" "${seal_path}")
