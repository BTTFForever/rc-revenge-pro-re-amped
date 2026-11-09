add_library(rc_ico_platform STATIC fiber.c vm_reserve.c state_reloc.c)
target_include_directories(rc_ico_platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(rc_ico_platform PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
