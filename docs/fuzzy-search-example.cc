// Example: fuzzy (approximate) search with marisa::Trie::fuzzy_search().
//
// Build with:
//   c++ -std=c++17 -Iinclude fuzzy-search-example.cc -lmarisa

#include <marisa.h>

#include <iostream>
#include <string>

int main() {
  // A small dictionary of normalized names, as used in data cleaning.
  marisa::Keyset keyset;
  keyset.push_back("cat");
  keyset.push_back("cut");
  keyset.push_back("cats");
  keyset.push_back("dog");
  keyset.push_back("猫");
  keyset.push_back("猫好");

  marisa::Trie trie;
  trie.build(keyset);

  // Find every key within one Unicode codepoint edit of the query.
  marisa::FuzzyStats stats;
  const std::vector<marisa::FuzzyMatch> matches =
      trie.fuzzy_search("cat", 1, &stats);

  for (const marisa::FuzzyMatch &match : matches) {
    std::cout << "id=" << match.id << " distance=" << match.distance << " key="
              << match.key << "\n";
  }
  std::cout << "visited_nodes=" << stats.visited_nodes
            << " decoded_codepoints=" << stats.decoded_codepoints << "\n";

  // Multi-byte characters count as one edit each: 猫 vs 猫好 has distance 1.
  for (const marisa::FuzzyMatch &match : trie.fuzzy_search("猫", 1)) {
    std::cout << "distance=" << match.distance << " key=" << match.key
              << "\n";
  }
  return 0;
}
