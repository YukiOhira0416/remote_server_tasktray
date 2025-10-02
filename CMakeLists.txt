cmake_minimum_required(VERSION 3.16)

project(remote_server_tasktray)

# C++17 標準を使用
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# uicの実行可能ファイルを見つける
find_program(UIC_EXECUTABLE uic)
if(NOT UIC_EXECUTABLE)
    message(FATAL_ERROR "uic not found. Please ensure Qt tools are installed and in PATH.")
endif()

# UIファイルを処理
set(UI_FILES
    UI/Main_UI.ui
)

# UIファイルからヘッダーファイルを生成
set(GENERATED_UI_HEADERS)
foreach(ui_file ${UI_FILES})
    get_filename_component(ui_basename ${ui_file} NAME_WE)
    set(ui_header "${CMAKE_CURRENT_BINARY_DIR}/ui_${ui_basename}.h")
    
    add_custom_command(
        OUTPUT ${ui_header}
        COMMAND ${UIC_EXECUTABLE} -o ${ui_header} ${CMAKE_CURRENT_SOURCE_DIR}/${ui_file}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${ui_file}
        COMMENT "Generating ${ui_header} from ${ui_file}"
        VERBATIM
    )
    
    list(APPEND GENERATED_UI_HEADERS ${ui_header})
endforeach()

# デバッグ情報: 生成されるヘッダーファイルを表示
message(STATUS "Generated UI headers: ${GENERATED_UI_HEADERS}")

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
    GPUManager.cpp
    DebugLog.cpp
    DisplayManager.cpp
    Globals.cpp
    RegistryHelper.cpp
    SharedMemoryHelper.cpp
    StringConversion.cpp
    Utility.cpp
    OverlayManager.cpp
)

# ヘッダーファイル
set(HEADERS
    remote_server_tasktray.h
    TaskTrayApp.h
    GPUManager.h
    GPUInfo.h
    DebugLog.h
    DisplayManager.h
    Globals.h
    RegistryHelper.h
    SharedMemoryHelper.h
    StringConversion.h
    Utility.h
    framework.h
    targetver.h
    Resource.h
    OverlayManager.h
)

# リソースファイル
set(RESOURCES
    remote_server_tasktray.rc
)

# 実行可能ファイルを作成
add_executable(${PROJECT_NAME} WIN32 ${SOURCES} ${HEADERS} ${RESOURCES} ${GENERATED_UI_HEADERS})

# UIヘッダー生成用のカスタムターゲットを作成
if(GENERATED_UI_HEADERS)
    add_custom_target(generate_ui_headers ALL DEPENDS ${GENERATED_UI_HEADERS})
    add_dependencies(${PROJECT_NAME} generate_ui_headers)
endif()

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
)

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

# Visual Studioのフィルター設定（オプション）
if(MSVC)
    source_group("Header Files" FILES ${HEADERS})
    source_group("Source Files" FILES ${SOURCES})
    source_group("Resource Files" FILES ${RESOURCES})
    source_group("UI Files" FILES ${UI_FILES})
    source_group("Generated Files" FILES ${GENERATED_UI_HEADERS})
endif()

# デバッグ情報出力
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "Target: ${PROJECT_NAME}")
