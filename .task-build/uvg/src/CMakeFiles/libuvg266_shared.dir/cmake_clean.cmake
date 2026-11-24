file(REMOVE_RECURSE
  "../lib/libuvg266.dylib"
  "../lib/libuvg266.pdb"
)

# Per-language clean rules from dependency scanning.
foreach(lang C)
  include(CMakeFiles/libuvg266_shared.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
