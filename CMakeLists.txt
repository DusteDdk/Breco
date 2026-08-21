cmake_minimum_required(VERSION 3.22)

project(breco VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/ui")

include(CTest)
include(GNUInstallDirs)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

if(WIN32)
    add_compile_definitions(WINVER=0x0A00 _WIN32_WINNT=0x0A00)
endif()

set(BRECO_ENABLE_LINUX_PRIVILEGED_OPEN OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(BRECO_ENABLE_LINUX_PRIVILEGED_OPEN ON)
    find_package(Qt6 REQUIRED COMPONENTS DBus Network)
endif()

set(BRECO_SOURCES
    src/main.cpp
    src/app/MainWindow.cpp
    src/brecolang/gui/BrecoLangLibrary.cpp
    src/brecolang/gui/BrecoLangPanel.cpp
    src/brecolang/gui/DecodedTreeModel.cpp
    src/panel/BitmapViewPanel.cpp
    src/panel/CurrentByteInfoPanel.cpp
    src/panel/DataViewByteAndBitmapPanel.cpp
    src/panel/DataViewImagePanel.cpp
    src/panel/DataViewShellPanel.cpp
    src/panel/HexViewControlsPanel.cpp
    src/panel/ResultsTablePanel.cpp
    src/panel/ScanControlsPanel.cpp
    src/panel/TextViewPanel.cpp
    src/scan/ScanController.cpp
    src/scan/ScanProgress.cpp
    src/scan/ScanWorker.cpp
    src/scan/ShiftTransform.cpp
    src/scan/MatchUtils.cpp
    src/model/ResultModel.cpp
    src/panel/MainTabsPanel.cpp
    src/view/BitmapViewWidget.cpp
    src/view/TextViewWidget.cpp
    src/io/FileEnumerator.cpp
    src/io/OpenFilePool.cpp
    src/io/ProtectedSourceOpener.cpp
    src/io/ShiftedWindowLoader.cpp
    src/image/EmbeddedImageScanner.cpp
    src/settings/AppSettings.cpp
    src/text/TextSequenceAnalyzer.cpp
    src/text/StringModeRules.cpp
)

if(BRECO_ENABLE_LINUX_PRIVILEGED_OPEN)
    list(APPEND BRECO_SOURCES
        src/io/PrivilegedFileOpener.cpp
        src/io/UDisks2DeviceOpener.cpp
    )
endif()

set(BRECO_HEADERS
    src/app/MainWindow.h
    src/brecolang/gui/BrecoLangLibrary.h
    src/brecolang/gui/BrecoLangPanel.h
    src/brecolang/gui/DecodedTreeModel.h
    src/panel/BitmapViewPanel.h
    src/panel/CurrentByteInfoPanel.h
    src/panel/DataViewByteAndBitmapPanel.h
    src/panel/DataViewImagePanel.h
    src/panel/DataViewShellPanel.h
    src/panel/HexViewControlsPanel.h
    src/panel/MainTabsPanel.h
    src/panel/ResultsTablePanel.h
    src/panel/ScanControlsPanel.h
    src/panel/TextViewPanel.h
    src/scan/ScanController.h
    src/scan/ScanProgress.h
    src/scan/ScanWorker.h
    src/scan/ShiftTransform.h
    src/scan/MatchUtils.h
    src/scan/ScanTypes.h
    src/scan/SpscQueue.h
    src/model/ResultTypes.h
    src/model/ResultModel.h
    src/view/BitmapViewWidget.h
    src/view/TextViewWidget.h
    src/io/FileEnumerator.h
    src/io/OpenFilePool.h
    src/io/ProtectedSourceOpener.h
    src/io/ShiftedWindowLoader.h
    src/image/EmbeddedImageScanner.h
    src/settings/AppSettings.h
    src/text/TextSequenceAnalyzer.h
    src/text/StringModeRules.h
)

if(BRECO_ENABLE_LINUX_PRIVILEGED_OPEN)
    list(APPEND BRECO_HEADERS
        src/io/PrivilegedFileOpener.h
        src/io/UDisks2DeviceOpener.h
    )
endif()

set(BRECO_UI
    ui/AboutDialog.ui
    ui/BitmapViewPanel.ui
    ui/CurrentByteInfo.ui
    ui/DataViewByteAndBitmap.ui
    ui/DataViewImage.ui
    ui/DataViewShell.ui
    ui/EditStack.ui
    ui/HexViewControlsPanel.ui
    ui/MainTabsPanel.ui
    ui/MainWindow.ui
    ui/ResultsTablePanel.ui
    ui/ScanControlsPanel.ui
    ui/TextViewPanel.ui
)

set(BRECO_RESOURCES
    res/icons.qrc
)

set(BRECO_QT_LIBS Qt6::Core Qt6::Gui Qt6::Widgets)
if(BRECO_ENABLE_LINUX_PRIVILEGED_OPEN)
    list(APPEND BRECO_QT_LIBS Qt6::DBus Qt6::Network)
endif()

add_library(brecolang_compiler STATIC
    src/brecolang/compiler/Compiler.cpp
    src/brecolang/compiler/Lexer.cpp
    src/brecolang/compiler/Parser.cpp
    src/brecolang/compiler/Compiler.h
    src/brecolang/compiler/Lexer.h
    src/brecolang/compiler/Parser.h
    src/brecolang/compiler/Source.h
    src/brecolang/compiler/Syntax.h
    src/brecolang/ir/BrecoProgram.h
)
target_include_directories(brecolang_compiler PUBLIC src)
target_link_libraries(brecolang_compiler PUBLIC Qt6::Core)

add_library(brecolang_runtime STATIC
    src/brecolang/render/OutformRenderer.cpp
    src/brecolang/render/RenderStore.cpp
    src/brecolang/runtime/ByteSource.cpp
    src/brecolang/runtime/DecodedData.cpp
    src/brecolang/runtime/Interpreter.cpp
    src/brecolang/runtime/JsonWriter.cpp
    src/brecolang/runtime/ByteSource.h
    src/brecolang/runtime/DecodedData.h
    src/brecolang/runtime/Interpreter.h
    src/brecolang/runtime/JsonWriter.h
    src/brecolang/runtime/ProbeScan.h
    src/brecolang/render/OutformRenderer.h
    src/brecolang/render/RenderStore.h
)
target_include_directories(brecolang_runtime PUBLIC src)
target_link_libraries(brecolang_runtime PUBLIC brecolang_compiler Qt6::Core)

qt_add_executable(breco
    ${BRECO_SOURCES}
    ${BRECO_HEADERS}
    ${BRECO_UI}
    ${BRECO_RESOURCES}
)

target_include_directories(breco PRIVATE src)
target_link_libraries(breco PRIVATE ${BRECO_QT_LIBS} brecolang_runtime)

if(WIN32)
    set_target_properties(breco PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

if(BRECO_ENABLE_LINUX_PRIVILEGED_OPEN)
    add_executable(breco_privileged_open_helper
        src/io/PrivilegedOpenHelper.cpp
    )
endif()

add_executable(brecodump
    src/cli/brecodump.cpp
)
target_include_directories(brecodump PRIVATE src)
target_link_libraries(brecodump PRIVATE Qt6::Core brecolang_runtime)

if(WIN32)
    set_target_properties(brecodump PROPERTIES WIN32_EXECUTABLE FALSE)
endif()

if(WIN32)
    # FILEVERSION/PRODUCTVERSION (the numeric VERSIONINFO header fields) must be
    # 4 comma-separated 16-bit integers, so the git short SHA can't go there;
    # it's used for the free-form StringFileInfo FileVersion/ProductVersion instead.
    find_package(Git QUIET)
    set(BRECO_GIT_SHORT_SHA "unknown")
    if(GIT_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE BRECO_GIT_SHORT_SHA_OUT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE BRECO_GIT_SHA_RESULT
            ERROR_QUIET
        )
        if(BRECO_GIT_SHA_RESULT EQUAL 0 AND BRECO_GIT_SHORT_SHA_OUT)
            set(BRECO_GIT_SHORT_SHA "${BRECO_GIT_SHORT_SHA_OUT}")
        endif()
    endif()

    set(BRECO_VERSIONINFO_COMPANY_NAME "DusteDs Stuff")
    set(BRECO_VERSIONINFO_FILE_DESCRIPTION "Binary recovery and forensics utility")
    set(BRECO_VERSIONINFO_FILE_VERSION_STR "${BRECO_GIT_SHORT_SHA}")
    set(BRECO_VERSIONINFO_LEGAL_COPYRIGHT "Copyleft forever WTFPL")
    set(BRECO_VERSIONINFO_PRODUCT_NAME "Breco Suite")
    set(BRECO_VERSIONINFO_PRODUCT_VERSION_STR "${BRECO_GIT_SHORT_SHA}")
    set(BRECO_VERSIONINFO_FILEVERSION_NUM "1,0,0,0")
    set(BRECO_VERSIONINFO_PRODUCTVERSION_NUM "1,0,0,0")

    # comments/internal_name/original_filename differ per executable;
    # pass original_filename as "" to omit it from the string table.
    function(breco_add_windows_executable_resources target icon_png comments internal_name original_filename)
        get_filename_component(icon_basename "${icon_png}" NAME_WE)
        set(icon_dir "${CMAKE_CURRENT_BINARY_DIR}/win-icons/${target}")
        set(icon_ico "${icon_dir}/${icon_basename}.ico")
        set(icon_rc "${icon_dir}/${target}.rc")

        find_program(BRECO_MAGICK_EXECUTABLE NAMES magick)
        if(NOT BRECO_MAGICK_EXECUTABLE)
            message(WARNING "ImageMagick (magick) not found; ${target} will be built without a custom icon.")
            return()
        endif()

        file(MAKE_DIRECTORY "${icon_dir}")

        set(version_strings
            "            VALUE \"CompanyName\", \"${BRECO_VERSIONINFO_COMPANY_NAME}\"\n"
            "            VALUE \"Comments\", \"${comments}\"\n"
            "            VALUE \"FileDescription\", \"${BRECO_VERSIONINFO_FILE_DESCRIPTION}\"\n"
            "            VALUE \"FileVersion\", \"${BRECO_VERSIONINFO_FILE_VERSION_STR}\"\n"
            "            VALUE \"InternalName\", \"${internal_name}\"\n"
            "            VALUE \"LegalCopyright\", \"${BRECO_VERSIONINFO_LEGAL_COPYRIGHT}\"\n"
        )
        if(NOT "${original_filename}" STREQUAL "")
            list(APPEND version_strings
                "            VALUE \"OriginalFilename\", \"${original_filename}\"\n"
            )
        endif()
        list(APPEND version_strings
            "            VALUE \"ProductName\", \"${BRECO_VERSIONINFO_PRODUCT_NAME}\"\n"
            "            VALUE \"ProductVersion\", \"${BRECO_VERSIONINFO_PRODUCT_VERSION_STR}\"\n"
        )
        string(JOIN "" version_strings_block ${version_strings})

        file(WRITE "${icon_rc}" "\
IDI_ICON1 ICON DISCARDABLE \"${icon_ico}\"

VS_VERSION_INFO VERSIONINFO
 FILEVERSION ${BRECO_VERSIONINFO_FILEVERSION_NUM}
 PRODUCTVERSION ${BRECO_VERSIONINFO_PRODUCTVERSION_NUM}
 FILEFLAGSMASK 0x3fL
 FILEFLAGS 0x0L
 FILEOS 0x40004L
 FILETYPE 0x1L
 FILESUBTYPE 0x0L
BEGIN
    BLOCK \"StringFileInfo\"
    BEGIN
        BLOCK \"040904b0\"
        BEGIN
${version_strings_block}        END
    END
    BLOCK \"VarFileInfo\"
    BEGIN
        VALUE \"Translation\", 0x409, 1200
    END
END
")

        add_custom_command(
            OUTPUT "${icon_ico}"
            COMMAND "${BRECO_MAGICK_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/res/${icon_png}"
                    -define icon:auto-resize=256,128,64,48,32,16
                    "${icon_ico}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/res/${icon_png}"
            COMMENT "Generating Windows icon ${icon_ico}"
            VERBATIM
        )

        target_sources(${target} PRIVATE "${icon_rc}")
        set_source_files_properties("${icon_rc}" PROPERTIES OBJECT_DEPENDS "${icon_ico}")
    endfunction()

    breco_add_windows_executable_resources(breco breco-icon.png
        "Breco - Graphical User Interface" "breco.exe" "")
    breco_add_windows_executable_resources(brecodump brecodump-icon.png
        "Breco - CLI for scripting" "BrecoDump" "brecodump.exe")
endif()

install(TARGETS breco brecodump
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/examples/"
    DESTINATION "examples"
)

if(BUILD_TESTING)
find_package(Qt6 REQUIRED COMPONENTS Test)

set(BRECO_APP_SOURCES ${BRECO_SOURCES})
list(REMOVE_ITEM BRECO_APP_SOURCES src/main.cpp)

qt_add_executable(breco_ui_integration_tests
    tests/mainwindow_integration_tests.cpp
    ${BRECO_APP_SOURCES}
    ${BRECO_HEADERS}
    ${BRECO_UI}
    ${BRECO_RESOURCES}
)
target_include_directories(breco_ui_integration_tests PRIVATE src)
# MSVC decorates member symbols differently by access, so the test and the
# MainWindow implementation must see the same declarations.
target_compile_definitions(
    breco_ui_integration_tests PRIVATE BRECO_MAINWINDOW_TEST_ACCESS)
target_link_libraries(
    breco_ui_integration_tests PRIVATE ${BRECO_QT_LIBS} Qt6::Test
                                       brecolang_runtime)

if(WIN32)
    set(BRECO_UI_TEST_REPORT
        "${CMAKE_CURRENT_BINARY_DIR}/breco_ui_integration_tests.txt")
    add_test(
        NAME breco_ui_integration_tests
        COMMAND breco_ui_integration_tests -o "${BRECO_UI_TEST_REPORT},txt")
else()
    add_test(NAME breco_ui_integration_tests COMMAND breco_ui_integration_tests)
endif()
set_tests_properties(breco_ui_integration_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

qt_add_executable(breco_unit_tests
    tests/unit_tests.cpp
    src/scan/MatchUtils.cpp
    src/scan/ScanProgress.cpp
    src/scan/ShiftTransform.cpp
    src/model/ResultModel.cpp
    src/io/FileEnumerator.cpp
    src/io/OpenFilePool.cpp
    src/io/ShiftedWindowLoader.cpp
    src/image/EmbeddedImageScanner.cpp
    src/settings/AppSettings.cpp
    src/text/TextSequenceAnalyzer.cpp
    src/text/StringModeRules.cpp
    src/view/BitmapViewWidget.cpp
    src/view/BitmapViewWidget.h
    src/view/TextViewWidget.cpp
    src/view/TextViewWidget.h
)
target_include_directories(breco_unit_tests PRIVATE src)
target_link_libraries(breco_unit_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)

add_test(NAME breco_unit_tests COMMAND breco_unit_tests)

qt_add_executable(brecolang_compiler_tests
    tests/brecolang_compiler_tests.cpp
)
target_compile_definitions(
    brecolang_compiler_tests PRIVATE BRECO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
    brecolang_compiler_tests PRIVATE brecolang_compiler Qt6::Core Qt6::Test)
add_test(NAME brecolang_compiler_tests COMMAND brecolang_compiler_tests)

qt_add_executable(brecolang_runtime_tests
    tests/brecolang_runtime_tests.cpp
)
target_link_libraries(
    brecolang_runtime_tests PRIVATE brecolang_runtime Qt6::Core Qt6::Test)
add_test(NAME brecolang_runtime_tests COMMAND brecolang_runtime_tests)

qt_add_executable(brecodump_cli_tests
    tests/brecodump_cli_tests.cpp
)
target_compile_definitions(
    brecodump_cli_tests PRIVATE BRECODUMP_PATH="$<TARGET_FILE:brecodump>")
target_link_libraries(brecodump_cli_tests PRIVATE Qt6::Core Qt6::Test)
add_dependencies(brecodump_cli_tests brecodump)

add_test(NAME brecodump_cli_tests COMMAND brecodump_cli_tests)
endif()

add_executable(breco_text_analysis_benchmark
    tests/text_analysis_benchmark.cpp
    src/text/TextSequenceAnalyzer.cpp
)
target_include_directories(breco_text_analysis_benchmark PRIVATE src)
target_link_libraries(breco_text_analysis_benchmark PRIVATE Qt6::Core)

add_executable(breco_scan_primitives_benchmark
    tests/scan_primitives_benchmark.cpp
    src/scan/MatchUtils.cpp
    src/scan/ShiftTransform.cpp
)
target_include_directories(breco_scan_primitives_benchmark PRIVATE src)
target_link_libraries(breco_scan_primitives_benchmark PRIVATE Qt6::Core)

find_program(PANDOC_EXECUTABLE pandoc)
if(NOT PANDOC_EXECUTABLE)
    message(WARNING "pandoc not found. Install pandoc to enable the doc_html target.")
else()
    set(DOC_HTML_OUTPUT_DIR "${CMAKE_BINARY_DIR}/doc_html")
    set(DOC_HTML_HEADER "${CMAKE_SOURCE_DIR}/docs/html/mermaid-header.html")

    set(DOC_PAGES
        "index|${CMAKE_SOURCE_DIR}/docs/html/index.md"
        "BrecoLang|${CMAKE_SOURCE_DIR}/docs/BrecoLang.md"
        "codemap|${CMAKE_SOURCE_DIR}/docs/codemap.md"
        "runtime-behavior|${CMAKE_SOURCE_DIR}/docs/runtime-behavior.md"
        "scan-and-io-behavior|${CMAKE_SOURCE_DIR}/docs/scan-and-io-behavior.md"
        "preview-cache-and-status|${CMAKE_SOURCE_DIR}/docs/preview-cache-and-status.md"
        "behavioral-invariants|${CMAKE_SOURCE_DIR}/docs/behavioral-invariants.md"
    )

    set(DOC_HTML_FILES)
    foreach(page_entry IN LISTS DOC_PAGES)
        string(REPLACE "|" ";" page_parts "${page_entry}")
        list(GET page_parts 0 page_name)
        list(GET page_parts 1 page_source)
        set(page_output "${DOC_HTML_OUTPUT_DIR}/${page_name}.html")
        list(APPEND DOC_HTML_FILES "${page_output}")

        add_custom_command(
            OUTPUT "${page_output}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${DOC_HTML_OUTPUT_DIR}"
            COMMAND ${PANDOC_EXECUTABLE}
                    --standalone
                    --toc
                    --from markdown
                    --to html5
                    --include-in-header "${DOC_HTML_HEADER}"
                    --output "${page_output}"
                    "${page_source}"
            DEPENDS "${page_source}" "${DOC_HTML_HEADER}"
            COMMENT "Generating documentation HTML: ${page_name}.html"
            VERBATIM
        )
    endforeach()

    add_custom_target(doc_html
        DEPENDS ${DOC_HTML_FILES}
        COMMENT "Building HTML documentation in ${DOC_HTML_OUTPUT_DIR}"
    )
endif()
