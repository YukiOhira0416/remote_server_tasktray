cmake_minimum_required(VERSION 3.16)

project(remote_server_tasktray)

# Qt6のパスを直接設定
list(APPEND CMAKE_PREFIX_PATH "C:/Qt/6.10.2/msvc2022_64")

# Qt6のパスを設定
if(NOT DEFINED Qt6_DIR)
    # 一般的なQt6インストールパスを検索
    set(QT6_SEARCH_PATHS
        "C:/Qt/6.10.2/msvc2022_64"
        "C:/Qt/6.10.0/msvc2022_64"
        "C:/Qt/6.9.3/msvc2022_64"
    )
    
    # 環境変数からQtパスを取得を試行
    if(DEFINED ENV{QT_DIR})
        list(INSERT QT6_SEARCH_PATHS 0 "$ENV{QT_DIR}")
    endif()
    
    if(DEFINED ENV{Qt6_DIR})
        list(INSERT QT6_SEARCH_PATHS 0 "$ENV{Qt6_DIR}")
    endif()
    
    if(DEFINED ENV{CMAKE_PREFIX_PATH})
        string(REPLACE ";" ";" ENV_CMAKE_PREFIX_PATH "$ENV{CMAKE_PREFIX_PATH}")
        list(INSERT QT6_SEARCH_PATHS 0 ${ENV_CMAKE_PREFIX_PATH})
    endif()
    
    # Qt6のインストールパスを検索
    foreach(qt_path ${QT6_SEARCH_PATHS})
        message(STATUS "Checking Qt6 path: ${qt_path}")
        if(EXISTS "${qt_path}/lib/cmake/Qt6")
            set(Qt6_DIR "${qt_path}/lib/cmake/Qt6")
            message(STATUS "Found Qt6 at: ${Qt6_DIR}")
            break()
        elseif(EXISTS "${qt_path}/cmake/Qt6")
            set(Qt6_DIR "${qt_path}/cmake/Qt6")
            message(STATUS "Found Qt6 at: ${Qt6_DIR}")
            break()
        elseif(EXISTS "${qt_path}")
            list(APPEND CMAKE_PREFIX_PATH "${qt_path}")
            message(STATUS "Added to CMAKE_PREFIX_PATH: ${qt_path}")
        endif()
    endforeach()
    
    # まだ見つからない場合、CMakeにヒントを提供
    if(NOT DEFINED Qt6_DIR)
        message(STATUS "Qt6 not found in standard locations. You may need to:")
        message(STATUS "1. Install Qt6 from https://www.qt.io/download")
        message(STATUS "2. Set Qt6_DIR environment variable to point to Qt6 cmake directory")
        message(STATUS "3. Set CMAKE_PREFIX_PATH to include Qt6 installation directory")
        message(STATUS "Example: cmake -DQt6_DIR=C:/Qt/6.10.0/msvc2022_64/lib/cmake/Qt6 ..")
    endif()
endif()

find_package(Qt6 COMPONENTS Widgets REQUIRED)

# Qt6のビルドツールを自動設定
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC OFF)  # 手動でUIファイルを処理するため無効化
set(CMAKE_AUTORCC ON)

# UICの検索パスを明示的に設定
set(CMAKE_AUTOUIC_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/UI)

# Qt6のインクルードディレクトリを設定
if(Qt6_FOUND)
    get_target_property(QT_INCLUDE_DIR Qt6::Core INTERFACE_INCLUDE_DIRECTORIES)
    message(STATUS "Qt6 include directories: ${QT_INCLUDE_DIR}")
endif()

# TaskTrayApp.cpp用のUI無効化
set_property(SOURCE TaskTrayApp.cpp PROPERTY SKIP_AUTOUIC ON)

# C++17 標準を使用
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# UIファイルを処理
set(UI_FILES
    UI/Main_UI.ui
)

# UIファイルのパスを絶対パスに設定
set(CMAKE_AUTOUIC_SEARCH_PATHS 
    ${CMAKE_CURRENT_SOURCE_DIR}/UI
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# UIファイルの場所を明示的に設定
set_property(SOURCE UI/Main_UI.ui PROPERTY SKIP_AUTOUIC ON)

# 手動でUIファイルを処理
find_program(QT_UIC_EXECUTABLE NAMES uic HINTS ${Qt6_DIR}/../../../bin)
if(QT_UIC_EXECUTABLE)
    message(STATUS "Found UIC: ${QT_UIC_EXECUTABLE}")
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ui_Main_UI.h
        COMMAND ${QT_UIC_EXECUTABLE} -o ${CMAKE_CURRENT_BINARY_DIR}/ui_Main_UI.h ${CMAKE_CURRENT_SOURCE_DIR}/UI/Main_UI.ui
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/UI/Main_UI.ui
        COMMENT "Generating ui_Main_UI.h from UI/Main_UI.ui"
        VERBATIM
    )
    
    # 生成されたUIヘッダーファイルをプロジェクトルートにコピー
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/ui_Main_UI.h
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/ui_Main_UI.h ${CMAKE_CURRENT_SOURCE_DIR}/ui_Main_UI.h
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/ui_Main_UI.h
        COMMENT "Copying ui_Main_UI.h to project root"
        VERBATIM
    )
    
    list(APPEND SOURCES ${CMAKE_CURRENT_BINARY_DIR}/ui_Main_UI.h)
endif()

# Windows Unicode文字セットを使用
add_definitions(-DUNICODE -D_UNICODE)

# Debug/Release設定に応じた前処理定義
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_definitions(-D_DEBUG -D_WINDOWS)
else()
    add_definitions(-DNDEBUG -D_WINDOWS)
endif()

# ソースファイル
set(SOURCES
    remote_server_tasktray.cpp
    TaskTrayApp.cpp
    DebugLog.cpp
    Globals.cpp
    SharedMemoryHelper.cpp
    StringConversion.cpp
    Utility.cpp
    OverlayManager.cpp
    DisplaySyncServer.cpp
)

# TaskTrayApp.cppファイルに対してAutoUICを無効化
set_property(SOURCE TaskTrayApp.cpp PROPERTY SKIP_AUTOUIC ON)

# ヘッダーファイル
set(HEADERS
    remote_server_tasktray.h
    TaskTrayApp.h
    DebugLog.h
    Globals.h
    SharedMemoryHelper.h
    StringConversion.h
    Utility.h
    framework.h
    targetver.h
    Resource.h
    OverlayManager.h
    DisplaySyncServer.h
)

# リソースファイル
set(RESOURCES
    remote_server_tasktray.rc
)

# 実行可能ファイルを作成
add_executable(${PROJECT_NAME} WIN32 ${SOURCES} ${HEADERS} ${RESOURCES})

# UIヘッダーファイルへの依存関係を追加
add_dependencies(${PROJECT_NAME} ui_header_generation)

# UIヘッダー生成用のカスタムターゲット
add_custom_target(ui_header_generation
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/ui_Main_UI.h ${CMAKE_CURRENT_SOURCE_DIR}/ui_Main_UI.h
    COMMENT "Generating and copying UI headers"
)

# Windows APIライブラリをリンク
target_link_libraries(${PROJECT_NAME}
    user32
    kernel32
    gdi32
    winspool
    comdlg32
    advapi32
    shell32
    ole32
    oleaut32
    uuid
    odbc32
    odbccp32
    dxgi
    d3d11
    Shcore
    Qt6::Widgets
    ws2_32
)

# コンパイラ固有の設定
if(MSVC)
    # Visual Studio固有の設定
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3          # 警告レベル3
        /EHsc        # 例外処理モデル
        /nologo      # MSVCロゴを表示しない
    )
    
    # Debug設定
    target_compile_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Debug>:/Zi>     # デバッグ情報生成
        $<$<CONFIG:Debug>:/Od>     # 最適化無効
    )
    
    # Release設定
    target_compile_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Release>:/O2>   # 最適化レベル2
        $<$<CONFIG:Release>:/Gy>   # 関数レベルリンク有効
    )
    
    # リンカー設定
    set_target_properties(${PROJECT_NAME} PROPERTIES
        LINK_FLAGS_DEBUG "/DEBUG"
        LINK_FLAGS_RELEASE "/OPT:REF /OPT:ICF"
    )
endif()

# MinGW/GCC用の設定
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall
        -Wextra
    )
    
    # Windows サブシステムでビルド
    target_link_options(${PROJECT_NAME} PRIVATE
        -mwindows
    )
endif()

# インクルードディレクトリの設定（現在のディレクトリと生成されたヘッダー用ディレクトリ）
target_include_directories(${PROJECT_NAME} PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/UI
)

# Qt6の自動生成ディレクトリを追加
if(TARGET Qt6::uic)
    get_target_property(QT_UIC_EXECUTABLE Qt6::uic IMPORTED_LOCATION)
    message(STATUS "Qt UIC executable: ${QT_UIC_EXECUTABLE}")
endif()

# Windows SDKのパスを探す（オプション）
if(WIN32)
    # Windows Kitsのパスを設定（必要に応じて）
    set(WINDOWS_KITS_DIR "C:/Program Files (x86)/Windows Kits/10")
    if(EXISTS ${WINDOWS_KITS_DIR})
        target_include_directories(${PROJECT_NAME} PRIVATE 
            "${WINDOWS_KITS_DIR}/Include/10.0.26100.0/um"
            "${WINDOWS_KITS_DIR}/Include/10.0.26100.0/shared"
            "${WINDOWS_KITS_DIR}/Include/10.0.26100.0/winrt"
        )
    endif()
endif()

# 出力ディレクトリの設定
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/Debug
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/Release
)

# windeployqtを使用してQt DLLを配置
if(WIN32)
    # Qt6のwindeployqtを探す
    find_program(WINDEPLOYQT_EXECUTABLE 
        NAMES windeployqt
        HINTS 
            "${Qt6_DIR}/../../../bin"
            "C:/Qt/6.10.2/msvc2022_64/bin"
            "C:/Qt/6.10.0/msvc2022_64/bin"
            "C:/Qt/6.9.3/msvc2022_64/bin"
    )
    
    if(WINDEPLOYQT_EXECUTABLE)
        message(STATUS "Found windeployqt: ${WINDEPLOYQT_EXECUTABLE}")
        
        # Debug用
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                --verbose 1
                --no-translations
                --no-system-d3d-compiler
                --no-opengl-sw
                "$<TARGET_FILE:${PROJECT_NAME}>"
            COMMENT "Running windeployqt for Debug build..."
            WORKING_DIRECTORY $<TARGET_FILE_DIR:${PROJECT_NAME}>
        )
    else()
        message(WARNING "windeployqt not found. Qt DLLs will need to be copied manually.")
        message(STATUS "Please add Qt bin directory to PATH or copy Qt DLLs manually.")
    endif()
endif()

# Visual Studioのフィルター設定（オプション）
if(MSVC)
    source_group("Header Files" FILES ${HEADERS})
    source_group("Source Files" FILES ${SOURCES})
    source_group("Resource Files" FILES ${RESOURCES})
    source_group("UI Files" FILES ${UI_FILES})
endif()

# デバッグ情報出力
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "Target: ${PROJECT_NAME}")
