add_library(manifold_core STATIC
    se3.cpp
    traverse.cpp
    holonomy.cpp
)
target_include_directories(manifold_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(manifold_core PUBLIC Eigen3::Eigen)
