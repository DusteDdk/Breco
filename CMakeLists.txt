cmake_minimum_required(VERSION 3.22)

project(breco VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/ui")

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Test)

set(BRECO_SOURCES
    src/main.cpp
    src/app/MainWindow.cpp
    src/panel/BitmapViewPanel.cpp
    src/panel/CurrentByteInfoPanel.cpp
    src/panel/ResultsTablePanel.cpp
    src/panel/ScanControlsPanel.cpp
    src/panel/TextViewPanel.cpp
    src/scan/ScanController.cpp
    src/scan/ScanWorker.cpp
    src/scan/ShiftTransform.cpp
    src/scan/MatchUtils.cpp
    src/model/ResultModel.cpp
    src/view/BitmapViewWidget.cpp
    src/view/TextViewWidget.cpp
    src/io/FileEnumerator.cpp
    src/io/OpenFilePool.cpp
    src/io/ShiftedWindowLoader.cpp
    src/settings/AppSettings.cpp
    src/text/TextSequenceAnalyzer.cpp
    src/text/StringModeRules.cpp
)

set(BRECO_HEADERS
    src/app/MainWindow.h
    src/panel/BitmapViewPanel.h
    src/panel/CurrentByteInfoPanel.h
    src/panel/ResultsTablePanel.h
    src/panel/ScanControlsPanel.h
    src/panel/TextViewPanel.h
    src/scan/ScanController.h
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
    src/io/ShiftedWindowLoader.h
    src/settings/AppSettings.h
    src/text/TextSequenceAnalyzer.h
    src/text/StringModeRules.h
)

set(BRECO_UI
    ui/AboutDialog.ui
    ui/BitmapViewPanel.ui
    ui/CurrentByteInfo.ui
    ui/EditStack.ui
    ui/MainWindow.ui
    ui/ResultsTablePanel.ui
    ui/ScanControlsPanel.ui
    ui/TextViewPanel.ui
    ui/ViewControls.ui
)

qt_add_executable(breco
    ${BRECO_SOURCES}
    ${BRECO_HEADERS}
    ${BRECO_UI}
)

target_include_directories(breco PRIVATE src)
target_link_libraries(breco PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)

enable_testing()

set(BRECO_APP_SOURCES ${BRECO_SOURCES})
list(REMOVE_ITEM BRECO_APP_SOURCES src/main.cpp)

qt_add_executable(breco_ui_integration_tests
    tests/mainwindow_integration_tests.cpp
    ${BRECO_APP_SOURCES}
    ${BRECO_HEADERS}
    ${BRECO_UI}
)
target_include_directories(breco_ui_integration_tests PRIVATE src)
target_link_libraries(
    breco_ui_integration_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)

add_test(NAME breco_ui_integration_tests COMMAND breco_ui_integration_tests)

add_executable(breco_unit_tests
    tests/unit_tests.cpp
    src/scan/MatchUtils.cpp
    src/scan/ShiftTransform.cpp
    src/model/ResultModel.cpp
    src/io/FileEnumerator.cpp
    src/io/OpenFilePool.cpp
    src/io/ShiftedWindowLoader.cpp
    src/text/TextSequenceAnalyzer.cpp
    src/text/StringModeRules.cpp
    src/view/BitmapViewWidget.cpp
)
target_include_directories(breco_unit_tests PRIVATE src)
target_link_libraries(breco_unit_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)

add_test(NAME breco_unit_tests COMMAND breco_unit_tests)

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
