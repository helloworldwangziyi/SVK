cmake_minimum_required(VERSION 3.22.1)

foreach(EVK_REQUIRED EVK_ROOT EVK_APP EVK_PLATFORM)
    if(NOT DEFINED ${EVK_REQUIRED} OR "${${EVK_REQUIRED}}" STREQUAL "")
        message(FATAL_ERROR "Missing -D${EVK_REQUIRED}=...")
    endif()
endforeach()

if(NOT EVK_APP MATCHES "^[A-Za-z0-9][A-Za-z0-9_-]*$")
    message(FATAL_ERROR "Invalid app directory name: ${EVK_APP}")
endif()

set(EVK_PLATFORMS android ios hongmeng)
list(FIND EVK_PLATFORMS "${EVK_PLATFORM}" EVK_PLATFORM_INDEX)
if(EVK_PLATFORM_INDEX EQUAL -1)
    message(FATAL_ERROR "Unsupported platform: ${EVK_PLATFORM}")
endif()

get_filename_component(EVK_ROOT "${EVK_ROOT}" ABSOLUTE)
file(TO_CMAKE_PATH "${EVK_ROOT}" EVK_ROOT)
set(EVK_APP_DIR "${EVK_ROOT}/samples/${EVK_APP}")
set(EVK_APP_CONFIG "${EVK_APP_DIR}/app.json")
if(NOT EXISTS "${EVK_APP_CONFIG}")
    message(FATAL_ERROR "App config not found: ${EVK_APP_CONFIG}")
endif()
file(READ "${EVK_APP_CONFIG}" APP_JSON)

function(EVK_READ_REQUIRED OUTPUT KEY)
    string(JSON VALUE ERROR_VARIABLE JSON_ERROR GET "${APP_JSON}" "${KEY}")
    if(NOT JSON_ERROR STREQUAL "NOTFOUND" OR "${VALUE}" STREQUAL "")
        message(FATAL_ERROR "Missing or empty '${KEY}' in ${EVK_APP_CONFIG}")
    endif()
    set(${OUTPUT} "${VALUE}" PARENT_SCOPE)
endfunction()

EVK_READ_REQUIRED(EVK_APP_NAME "name")
EVK_READ_REQUIRED(EVK_APP_DISPLAY_NAME "displayName")
EVK_READ_REQUIRED(EVK_APP_IDENTIFIER "identifier")
EVK_READ_REQUIRED(EVK_APP_VERSION_NAME "versionName")
EVK_READ_REQUIRED(EVK_APP_VERSION_CODE "versionCode")

if(NOT EVK_APP_NAME MATCHES "^[A-Za-z0-9][A-Za-z0-9._-]*$")
    message(FATAL_ERROR "App name must be safe for build products: ${EVK_APP_NAME}")
endif()
if(NOT EVK_APP_IDENTIFIER MATCHES "^[A-Za-z0-9_]+(\\.[A-Za-z0-9_]+)+$")
    message(FATAL_ERROR "Invalid app identifier: ${EVK_APP_IDENTIFIER}")
endif()
if(NOT EVK_APP_VERSION_NAME MATCHES "^[0-9]+(\\.[0-9]+)*$")
    message(FATAL_ERROR "Invalid versionName: ${EVK_APP_VERSION_NAME}")
endif()
if(NOT EVK_APP_VERSION_CODE MATCHES "^[0-9]+$")
    message(FATAL_ERROR "Invalid versionCode: ${EVK_APP_VERSION_CODE}")
endif()
if(EVK_APP_DISPLAY_NAME MATCHES "[\"']")
    message(FATAL_ERROR "displayName cannot contain quotes")
endif()

string(JSON SOURCE_COUNT LENGTH "${APP_JSON}" "sources")
if(SOURCE_COUNT LESS 1)
    message(FATAL_ERROR "App sources cannot be empty: ${EVK_APP_CONFIG}")
endif()

set(EVK_APP_CMAKE_SOURCES "")
set(EVK_APP_XCODE_BUILD_FILES "")
set(EVK_APP_XCODE_FILE_REFERENCES "")
set(EVK_APP_XCODE_GROUP_CHILDREN "")
set(EVK_APP_XCODE_SOURCES "")
set(INDEX 0)
while(INDEX LESS SOURCE_COUNT)
    string(JSON REL_SOURCE GET "${APP_JSON}" "sources" ${INDEX})
    if(NOT REL_SOURCE MATCHES "\\.cpp$")
        message(FATAL_ERROR "Only .cpp app sources are supported: ${REL_SOURCE}")
    endif()
    if(IS_ABSOLUTE "${REL_SOURCE}" OR REL_SOURCE MATCHES "(^|/)\\.\\.(/|$)")
        message(FATAL_ERROR "App source must stay inside its app directory: ${REL_SOURCE}")
    endif()

    get_filename_component(SOURCE_ABS "${EVK_APP_DIR}/${REL_SOURCE}" ABSOLUTE)
    if(NOT EXISTS "${SOURCE_ABS}")
        message(FATAL_ERROR "App source not found: ${SOURCE_ABS}")
    endif()
    file(TO_CMAKE_PATH "${SOURCE_ABS}" SOURCE_ABS)
    get_filename_component(SOURCE_NAME "${REL_SOURCE}" NAME)

    if(INDEX EQUAL 0)
        string(APPEND EVK_APP_CMAKE_SOURCES "\"${SOURCE_ABS}\"")
    else()
        string(APPEND EVK_APP_CMAKE_SOURCES "\n    \"${SOURCE_ABS}\"")
    endif()

    math(EXPR SERIAL "${INDEX} + 1")
    string(LENGTH "${SERIAL}" SERIAL_LENGTH)
    math(EXPR PAD_LENGTH "21 - ${SERIAL_LENGTH}")
    string(SUBSTRING "000000000000000000000" 0 ${PAD_LENGTH} PAD)
    set(FILE_ID "ECA${PAD}${SERIAL}")
    set(BUILD_ID "ECB${PAD}${SERIAL}")

    string(APPEND EVK_APP_XCODE_BUILD_FILES
        "\t\t${BUILD_ID} /* ${SOURCE_NAME} in Sources */ = {isa = PBXBuildFile; fileRef = ${FILE_ID} /* ${SOURCE_NAME} */; };\n")
    string(APPEND EVK_APP_XCODE_FILE_REFERENCES
        "\t\t${FILE_ID} /* ${SOURCE_NAME} */ = {isa = PBXFileReference; lastKnownFileType = sourcecode.cpp.cpp; path = \"${REL_SOURCE}\"; sourceTree = \"<group>\"; };\n")
    string(APPEND EVK_APP_XCODE_GROUP_CHILDREN
        "\t\t\t\t${FILE_ID} /* ${SOURCE_NAME} */,\n")
    string(APPEND EVK_APP_XCODE_SOURCES
        "\t\t\t\t${BUILD_ID} /* ${SOURCE_NAME} in Sources */,\n")

    math(EXPR INDEX "${INDEX} + 1")
endwhile()

set(HOST_DIR "${EVK_ROOT}/platform/${EVK_PLATFORM}/host")
if(NOT EXISTS "${HOST_DIR}")
    message(FATAL_ERROR "Host template not found: ${HOST_DIR}")
endif()
set(EVK_GENERATED_DIR "${EVK_ROOT}/build/apps/${EVK_APP}/${EVK_PLATFORM}")
file(MAKE_DIRECTORY "${EVK_GENERATED_DIR}")
file(COPY "${HOST_DIR}/" DESTINATION "${EVK_GENERATED_DIR}"
    PATTERN ".idea" EXCLUDE
    PATTERN ".gradle" EXCLUDE
    PATTERN ".cxx" EXCLUDE
    PATTERN ".DS_Store" EXCLUDE
    PATTERN ".hvigor" EXCLUDE
    PATTERN "build" EXCLUDE
    PATTERN "DerivedData" EXCLUDE
    PATTERN "oh_modules" EXCLUDE
    PATTERN "xcuserdata" EXCLUDE
)

if(EVK_PLATFORM STREQUAL "android")
    configure_file("${HOST_DIR}/settings.gradle" "${EVK_GENERATED_DIR}/settings.gradle" @ONLY)
    configure_file("${HOST_DIR}/app/build.gradle" "${EVK_GENERATED_DIR}/app/build.gradle" @ONLY)
    configure_file("${HOST_DIR}/app/src/main/cpp/CMakeLists.txt"
                   "${EVK_GENERATED_DIR}/app/src/main/cpp/CMakeLists.txt" @ONLY)
    file(REMOVE "${EVK_GENERATED_DIR}/app/src/main/res/values/strings.xml")
elseif(EVK_PLATFORM STREQUAL "ios")
    configure_file("${HOST_DIR}/EVKApp.xcodeproj/project.pbxproj"
                   "${EVK_GENERATED_DIR}/EVKApp.xcodeproj/project.pbxproj" @ONLY)
    configure_file("${HOST_DIR}/EVKApp.xcodeproj/xcshareddata/xcschemes/EVKApp.xcscheme"
                   "${EVK_GENERATED_DIR}/EVKApp.xcodeproj/xcshareddata/xcschemes/EVKApp.xcscheme" @ONLY)
    configure_file("${HOST_DIR}/EVKApp/Info.plist"
                   "${EVK_GENERATED_DIR}/EVKApp/Info.plist" @ONLY)
else()
    configure_file("${HOST_DIR}/AppScope/app.json5"
                   "${EVK_GENERATED_DIR}/AppScope/app.json5" @ONLY)
    configure_file("${HOST_DIR}/AppScope/resources/base/element/string.json"
                   "${EVK_GENERATED_DIR}/AppScope/resources/base/element/string.json" @ONLY)
    configure_file("${HOST_DIR}/entry/src/main/resources/base/element/string.json"
                   "${EVK_GENERATED_DIR}/entry/src/main/resources/base/element/string.json" @ONLY)
    configure_file("${HOST_DIR}/entry/src/main/cpp/CMakeLists.txt"
                   "${EVK_GENERATED_DIR}/entry/src/main/cpp/CMakeLists.txt" @ONLY)
    configure_file("${HOST_DIR}/oh-package.json5"
                   "${EVK_GENERATED_DIR}/oh-package.json5" @ONLY)
endif()

message(STATUS "Generated ${EVK_APP}/${EVK_PLATFORM}: ${EVK_GENERATED_DIR}")
