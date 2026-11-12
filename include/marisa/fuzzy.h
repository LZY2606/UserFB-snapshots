#ifndef MARISA_FUZZY_H_
#define MARISA_FUZZY_H_

#include <cstddef>
#include <string>

namespace marisa {

// A single result of Trie::fuzzy_search().
struct FuzzyMatch {
  // The key ID, identical to the ID assigned to lookup() results.
  std::size_t id = 0;
  // The original bytes of the matched key.
  std::string key;
  // The Unicode scalar Levenshtein distance between the query and the key.
  unsigned distance = 0;
};

// Counters describing the work done by one Trie::fuzzy_search() call.
// Both members are reset to zero at the beginning of every call.
struct FuzzyStats {
  // Number of nodes of the top-level trie actually expanded by the search.
  std::size_t visited_nodes = 0;
  // Number of dictionary codepoints fully decoded in order to advance the
  // distance state.
  std::size_t decoded_codepoints = 0;
};

}  // namespace marisa

#endif  // MARISA_FUZZY_H_
