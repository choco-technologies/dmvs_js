set(DMOD_MODULE_NAME        jsdump)
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")
set(DMOD_STACK_SIZE         32768)
set(DMOD_PRIORITY           1)
set(DMOD_MAL_IMPLS          )

dmod_add_executable(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    jsdump.c
)

target_link_libraries(${DMOD_MODULE_NAME} dmvs_js_if)

# dmvs_js.h declares the compiler too: dmvsi's headers
dmod_link_modules(${DMOD_MODULE_NAME}
    dmvsi@>=0.4
)
