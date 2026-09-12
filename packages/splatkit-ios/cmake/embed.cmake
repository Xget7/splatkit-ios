# splatkit_embed_text(<target> <file> <symbol>): generates embedded/<symbol>.h declaring
# `inline constexpr const char <symbol>[]` with the file's text, as a target the library
# depends on, so a shader edit rebuilds it.
function(splatkit_embed_text target file symbol)
  set(out ${CMAKE_CURRENT_BINARY_DIR}/embedded/${symbol}.h)
  get_filename_component(shader_dir ${file} DIRECTORY)
  file(GLOB_RECURSE shader_sources CONFIGURE_DEPENDS
    ${shader_dir}/*.metal ${shader_dir}/*.metalh)
  add_custom_command(
    OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${file} -DOUTPUT=${out} -DSYMBOL=${symbol}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-text.cmake
    DEPENDS ${file} ${shader_sources} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-text.cmake
    COMMENT "Embedding ${file}"
  )
  add_custom_target(${target} DEPENDS ${out})
endfunction()
