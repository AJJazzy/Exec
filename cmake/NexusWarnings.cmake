# Shared compiler settings for Nexus-owned targets (not applied to third-party code).
function(nexus_configure_target target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wno-unused-parameter
            -Wno-missing-field-initializers
            $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-Wnon-virtual-dtor>
        )
        if(NEXUS_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
