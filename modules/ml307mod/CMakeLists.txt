
# ESP-IDF component CMakeLists.txt for ml307mod
# This is needed for ESP-IDF to recognize this as a component
# and link against the esp-ml307 library

idf_component_register(
    SRCS 
        "ml307mod.c"
        "ml307call.cpp"
    
    INCLUDE_DIRS 
        "."

    REQUIRES 
        esp-ml307
)
