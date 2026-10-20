# splatkit_embed_text(<target> <file> <symbol> <shader_root>): generates embedded/<symbol>.h
# declaring `inline constexpr const char <symbol>[]` with the file's text, as a target the
# library depends on. Every .metal and .metalh under <shader_root> is a dependency, so a
# shader edit in any job folder rebuilds it.
function(splatkit_embed_text target file symbol shader_root)
  set(out ${CMAKE_CURRENT_BINARY_DIR}/embedded/${symbol}.h)
  file(GLOB_RECURSE shader_sources CONFIGURE_DEPENDS
    ${shader_root}/*.metal ${shader_root}/*.metalh)
  add_custom_command(
    OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${file} -DOUTPUT=${out} -DSYMBOL=${symbol}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-text.cmake
    DEPENDS ${file} ${shader_sources} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-text.cmake
    COMMENT "Embedding ${file}"
  )
  add_custom_target(${target} DEPENDS ${out})
endfunction()
