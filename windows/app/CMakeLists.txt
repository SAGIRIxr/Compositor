find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets)
find_package(Qt6 6.4 QUIET COMPONENTS Test)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# 界面代码编成静态库，供程序本体和界面冒烟测试共用。
add_library(compositor_ui STATIC
    Session.cpp Session.h
    QtBridge.cpp QtBridge.h
    CanvasView.cpp CanvasView.h
    LayersPanel.cpp LayersPanel.h
    MainWindow.cpp MainWindow.h
    Dialogs.cpp Dialogs.h
    AdjustmentDialog.cpp AdjustmentDialog.h
    CurveEditor.cpp CurveEditor.h
    ToolState.h
)
target_include_directories(compositor_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(compositor_ui PUBLIC compositor_core Qt6::Core Qt6::Gui Qt6::Widgets)
target_compile_definitions(compositor_ui PUBLIC COMPOSITOR_VERSION="${PROJECT_VERSION}")

set(APP_SOURCES main.cpp resources.qrc)
if(WIN32)
    list(APPEND APP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/../packaging/compositor.rc)
endif()
qt_add_executable(Compositor ${APP_SOURCES})
target_link_libraries(Compositor PRIVATE compositor_ui)
set_target_properties(Compositor PROPERTIES WIN32_EXECUTABLE ON OUTPUT_NAME Compositor)
install(TARGETS Compositor RUNTIME DESTINATION .)

if(COMPOSITOR_BUILD_TESTS AND TARGET Qt6::Test)
    qt_add_executable(compositor_ui_smoke smoke_test.cpp resources.qrc)
    target_link_libraries(compositor_ui_smoke PRIVATE compositor_ui Qt6::Test)
    add_test(NAME compositor_ui_smoke COMMAND compositor_ui_smoke)
    # 无显示器环境（CI）用离屏平台。
    set_tests_properties(compositor_ui_smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
