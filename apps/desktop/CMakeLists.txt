add_executable(qcae-desktop main.cpp)
target_link_libraries(qcae-desktop PRIVATE qcae_desktop_ui)
qcae_warnings(qcae-desktop)
vtk_module_autoinit(TARGETS qcae-desktop MODULES ${VTK_LIBRARIES})
