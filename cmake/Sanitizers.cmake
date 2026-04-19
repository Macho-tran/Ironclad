# Apply ASan + UBSan when IRONCLAD_SANITIZE is ON.
if(IRONCLAD_SANITIZE)
    if(MSVC)
        message(WARNING "IRONCLAD_SANITIZE: only ASan is enabled on MSVC")
        add_compile_options(/fsanitize=address)
    else()
        add_compile_options(
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all
        )
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()
