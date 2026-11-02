cmake_minimum_required(VERSION 3.22)
set(TARGET ToolkitMod)
project(${TARGET})

add_library(${TARGET} SHARED
        src/dllmain.cpp
        src/ToolkitMod.cpp
        src/Session.cpp
        src/LevelGuards.cpp
        src/SelfTest.cpp
        src/Trainer/TargetResolver.cpp
        src/Trainer/TrainerEngine.cpp
        src/Trainer/TrainerExpressions.cpp
        src/Trainer/Inspect.cpp
        src/Teleport/Waypoints.cpp
        src/Teleport/WaypointRenderer.cpp
        src/Teleport/WaypointMarkers.cpp
        src/UI/TrainerUI.cpp
        src/UI/TeleportUI.cpp
        src/UI/LevelsUI.cpp
        src/UI/EngineUI.cpp
)

target_include_directories(${TARGET} PRIVATE "include")
target_link_libraries(${TARGET} PRIVATE ImGui)
target_link_libraries(${TARGET} PUBLIC UE4SS)
