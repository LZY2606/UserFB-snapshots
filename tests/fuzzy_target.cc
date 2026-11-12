// Tests for marisa::Trie::fuzzy_search(). The reference distance below is a
// small independent implementation used only to cross-check the library.

#include <marisa.h>

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "suite.h"

namespace {

// Strict UTF-8 decoder used as a reference. Returns false on invalid input.
bool ref_decode(const std::string &s, std::vector<char32_t> *out) {
  out->clear();
  std::size_t i = 0;
  while (i < s.size()) {
    const unsigned char lead = static_cast<unsigned char>(s[i]);
    char32_t cp;
    std::size_t extra;
    char32_t min_cp;
    if (lead < 0x80) {
      cp = lead;
      extra = 0;
      min_cp = 0;
    } else if (lead < 0xC2) {
      return false;
    } else if (lead < 0xE0) {
      cp = lead & 0x1F;
      extra = 1;
      min_cp = 0x80;
    } else if (lead < 0xF0) {
      cp = lead & 0x0F;
      extra = 2;
      min_cp = 0x800;
    } else if (lead < 0xF5) {
      cp = lead & 0x07;
      extra = 3;
      min_cp = 0x10000;
    } else {
      return false;
    }
    if (s.size() - i - 1 < extra) {
      return false;
    }
    for (std::size_t j = 1; j <= extra; ++j) {
      const unsigned char cont = static_cast<unsigned char>(s[i + j]);
      if ((cont & 0xC0) != 0x80) {
        return false;
      }
      cp = (cp << 6) | (cont & 0x3F);
    }
    if ((cp < min_cp) || ((cp & 0xFFFFF800) == 0xD800) || (cp > 0x10FFFF)) {
      return false;
    }
    out->push_back(cp);
    i += extra + 1;
  }
  return true;
}

// Reference Levenshtein distance over Unicode codepoints.
unsigned ref_distance(const std::string &a, const std::string &b) {
  std::vector<char32_t> x;
  std::vector<char32_t> y;
  if (!ref_decode(a, &x) || !ref_decode(b, &y)) {
    return ~0U;
  }
  std::vector<unsigned> prev(y.size() + 1);
  std::vector<unsigned> cur(y.size() + 1);
  for (std::size_t j = 0; j <= y.size(); ++j) {
    prev[j] = static_cast<unsigned>(j);
  }
  for (std::size_t i = 1; i <= x.size(); ++i) {
    cur[0] = static_cast<unsigned>(i);
    for (std::size_t j = 1; j <= y.size(); ++j) {
      const unsigned cost = (x[i - 1] == y[j - 1]) ? 0U : 1U;
      cur[j] = std::min(std::min(prev[j] + 1, cur[j - 1] + 1),
                        prev[j - 1] + cost);
    }
    prev.swap(cur);
  }
  return prev[y.size()];
}

marisa::Trie build_trie(const std::vector<std::string> &keys, int flags) {
  marisa::Keyset keyset;
  for (const auto &key : keys) {
    keyset.push_back(key.data(), key.size());
  }
  marisa::Trie trie;
  trie.build(keyset, flags);
  return trie;
}

// Computes the expected result by scanning all keys with the reference
// distance and looking up key IDs through the public lookup() API.
std::vector<marisa::FuzzyMatch> brute_force(const marisa::Trie &trie,
                                            const std::vector<std::string> &keys,
                                            const std::string &query,
                                            unsigned max_edits) {
  std::vector<marisa::FuzzyMatch> out;
  for (const auto &key : keys) {
    std::vector<char32_t> cps;
    if (!ref_decode(key, &cps)) {
      continue;  // Keys that are not valid UTF-8 are excluded.
    }
    const unsigned distance = ref_distance(query, key);
    if (distance > max_edits) {
      continue;
    }
    marisa::Agent agent;
    agent.set_query(key.data(), key.size());
    if (!trie.lookup(agent)) {
      continue;
    }
    marisa::FuzzyMatch match;
    match.id = agent.key().id();
    match.key = key;
    match.distance = distance;
    out.push_back(std::move(match));
  }
  std::sort(out.begin(), out.end(),
            [](const marisa::FuzzyMatch &lhs, const marisa::FuzzyMatch &rhs) {
              if (lhs.distance != rhs.distance) {
                return lhs.distance < rhs.distance;
              }
              return lhs.key < rhs.key;
            });
  return out;
}

bool same_matches(const std::vector<marisa::FuzzyMatch> &lhs,
                  const std::vector<marisa::FuzzyMatch> &rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    if ((lhs[i].id != rhs[i].id) || (lhs[i].key != rhs[i].key) ||
        (lhs[i].distance != rhs[i].distance)) {
      return false;
    }
  }
  return true;
}

// Compares only keys and distances, ignoring key IDs. IDs are assigned in
// terminal-node order, so they change when unrelated keys are added.
bool same_words(const std::vector<marisa::FuzzyMatch> &lhs,
                const std::vector<marisa::FuzzyMatch> &rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    if ((lhs[i].key != rhs[i].key) || (lhs[i].distance != rhs[i].distance)) {
      return false;
    }
  }
  return true;
}

bool is_sorted(const std::vector<marisa::FuzzyMatch> &matches) {
  for (std::size_t i = 1; i < matches.size(); ++i) {
    if (matches[i - 1].distance > matches[i].distance) {
      return false;
    }
    if ((matches[i - 1].distance == matches[i].distance) &&
        !(matches[i - 1].key < matches[i].key)) {
      return false;
    }
  }
  return true;
}

template <typename Exception, typename Func>
bool throws(Func func) {
  try {
    func();
  } catch (const Exception &) {
    return true;
  }
  return false;
}

void unicode(Check &check) {
  const std::vector<std::string> keys = {
      "",        "cat",   "cut",   "ct",    "cats", "cast", "dog",
      "猫",      "猫好",  "好",    "🐱",    "cafe", "café",
      std::string("a\0b", 3),
  };
  const marisa::Trie trie =
      build_trie(keys, 3 | MARISA_TEXT_TAIL | MARISA_LABEL_ORDER);

  // Exact and one-edit matches, ordered by (distance, unsigned byte order).
  const std::vector<marisa::FuzzyMatch> cat = trie.fuzzy_search("cat", 1);
  check.expect(same_matches(cat, brute_force(trie, keys, "cat", 1)));
  check.expect(cat.size() == 5);
  check.expect((cat[0].key == "cat") && (cat[0].distance == 0));
  check.expect((cat[1].key == "cast") && (cat[2].key == "cats") &&
               (cat[3].key == "ct") && (cat[4].key == "cut"));

  // A multi-byte character is a single edit, not one edit per byte.
  const std::vector<marisa::FuzzyMatch> neko = trie.fuzzy_search("猫", 1);
  check.expect(same_matches(neko, brute_force(trie, keys, "猫", 1)));
  check.expect(neko.size() == 5);  // 猫, "", 好, 猫好, 🐱.
  check.expect((neko[0].key == "猫") && (neko[0].distance == 0));
  check.expect(neko[1].key.empty() && (neko[1].distance == 1));
  check.expect((neko[2].key == "好") && (neko[2].distance == 1));
  check.expect((neko[3].key == "猫好") && (neko[3].distance == 1));
  check.expect((neko[4].key == "🐱") && (neko[4].distance == 1));

  // Empty query and empty key.
  check.expect(trie.fuzzy_search("", 0).size() == 1);
  check.expect(trie.fuzzy_search("", 0)[0].key.empty());
  check.expect(trie.fuzzy_search("", 1).size() == 4);  // "", 好, 猫, 🐱.
  check.expect(same_matches(trie.fuzzy_search("", 2),
                            brute_force(trie, keys, "", 2)));

  // An embedded NUL is U+0000, not a terminator.
  const std::string nul_key("a\0b", 3);
  const std::vector<marisa::FuzzyMatch> nul = trie.fuzzy_search(nul_key, 0);
  check.expect((nul.size() == 1) && (nul[0].key == nul_key));
  check.expect(same_matches(trie.fuzzy_search("ab", 1),
                            brute_force(trie, keys, "ab", 1)));

  // Repeated queries on the same object give consistent results.
  for (const char *query : {"cat", "猫", "cut", "", "café", "cat"}) {
    check.expect(same_matches(trie.fuzzy_search(query, 2),
                              brute_force(trie, keys, query, 2)));
  }

  // The query must be valid UTF-8 and max_edits must be 0, 1 or 2.
  check.expect(throws<std::invalid_argument>(
      [&] { trie.fuzzy_search("\xFF", 1); }));
  check.expect(throws<std::invalid_argument>(
      [&] { trie.fuzzy_search("\xC0\x80", 1); }));  // Overlong.
  check.expect(throws<std::invalid_argument>(
      [&] { trie.fuzzy_search("\xED\xA0\x80", 1); }));  // Surrogate.
  check.expect(throws<std::invalid_argument>(
      [&] { trie.fuzzy_search("\xE6\x97", 1); }));  // Truncated.
  check.expect(throws<std::out_of_range>(
      [&] { trie.fuzzy_search("cat", 3); }));
  check.expect(throws<std::logic_error>(
      [] { marisa::Trie().fuzzy_search("cat", 1); }));
}

void compressed(Check &check) {
  // "prefixéalpha" and "prefixèalpha" share the byte prefix "prefix\xC3",
  // so the first byte of é/è is compressed into a link and the codepoint is
  // split across the segment boundary. 🐱 (F0 9F 90 B1) and 🐶 (F0 9F 90 B6)
  // share three of their four bytes. "g\xFF\xFEod" is not valid UTF-8.
  const std::vector<std::string> keys = {
      "prefixéalpha", "prefixèalpha", "prefixéomega", "prefix",
      "🐱🐱🐱🐱", "🐱🐱🐱🐶", "🐱🐱🐶🐱",
      "日本語", "日本語辞典", "日本人", "日本", "日記",
      "good", "goods", "god", std::string("g\xFF\xFEod", 5),
  };
  const int orders[] = {MARISA_LABEL_ORDER, MARISA_WEIGHT_ORDER};
  const int tails[] = {MARISA_TEXT_TAIL, MARISA_BINARY_TAIL};
  const std::vector<std::string> queries = {
      "prefixéalpha", "prefixèalpha", "🐱🐱🐱🐱", "日本語", "good", "god",
  };
  for (const int order : orders) {
    for (const int tail : tails) {
      for (int num_tries = 1; num_tries <= 3; ++num_tries) {
        const marisa::Trie trie =
            build_trie(keys, num_tries | tail | order);
        for (const std::string &query : queries) {
          for (unsigned max_edits = 0; max_edits <= 2; ++max_edits) {
            const std::vector<marisa::FuzzyMatch> got =
                trie.fuzzy_search(query, max_edits);
            check.expect(
                same_matches(got, brute_force(trie, keys, query, max_edits)));
            check.expect(is_sorted(got));
          }
        }
      }
    }
  }

  // A codepoint split across a compressed segment boundary is one edit.
  const marisa::Trie trie =
      build_trie(keys, 3 | MARISA_TEXT_TAIL | MARISA_LABEL_ORDER);
  const std::vector<marisa::FuzzyMatch> acute =
      trie.fuzzy_search("prefixéalpha", 1);
  check.expect(acute.size() == 2);  // éalpha, èalpha.
  check.expect((acute[1].key == "prefixèalpha") &&
               (acute[1].distance == 1));
  const std::vector<marisa::FuzzyMatch> cats = trie.fuzzy_search("🐱🐱🐱🐱", 1);
  check.expect(cats.size() == 3);  // 🐱🐱🐱🐱, 🐱🐱🐱🐶, 🐱🐱🐶🐱.
  check.expect((cats[0].key == "🐱🐱🐱🐱") && (cats[0].distance == 0));

  // The invalid key is stored and lookup() finds it, but fuzzy_search
  // excludes it while its valid siblings still match.
  marisa::Agent agent;
  agent.set_query("g\xFF\xFEod", 5);
  check.expect(trie.lookup(agent));
  const std::vector<marisa::FuzzyMatch> good = trie.fuzzy_search("good", 2);
  check.expect(good.size() == 3);  // good, god, goods.
  for (const auto &match : good) {
    check.expect(match.key != std::string("g\xFF\xFEod", 5));
  }
}

void pruning(Check &check) {
  const std::vector<std::string> base_keys = {
      "cat", "cut", "cart", "catnap", "catalog", "dog", "scatter",
  };
  const marisa::Trie small =
      build_trie(base_keys, 3 | MARISA_TEXT_TAIL | MARISA_LABEL_ORDER);
  marisa::FuzzyStats small_stats;
  const std::vector<marisa::FuzzyMatch> small_result =
      small.fuzzy_search("cat", 1, &small_stats);
  check.expect(same_matches(small_result,
                            brute_force(small, base_keys, "cat", 1)));

  // 4096 keys sharing a long prefix that is far away from the query must be
  // pruned by the distance lower bound, not scanned.
  std::vector<std::string> many_keys = base_keys;
  for (int i = 0; i < 4096; ++i) {
    many_keys.push_back("zzzzzzzzzzzz" + std::to_string(i));
  }
  const int orders[] = {MARISA_LABEL_ORDER, MARISA_WEIGHT_ORDER};
  for (const int order : orders) {
    const marisa::Trie big = build_trie(many_keys, 3 | MARISA_TEXT_TAIL | order);
    marisa::FuzzyStats stats;
    const std::vector<marisa::FuzzyMatch> result =
        big.fuzzy_search("cat", 1, &stats);
    // The distant "zzz..." keys change neither the matching words nor their
    // distances.
    check.expect(same_words(result, small_result));
    check.expect(same_matches(result, brute_force(big, many_keys, "cat", 1)));
    check.expect(stats.visited_nodes < 512);
    check.expect(stats.decoded_codepoints < 512);
    check.expect((stats.visited_nodes > 0) && (stats.decoded_codepoints > 0));

    // The counters are reset at every call, not accumulated.
    marisa::FuzzyStats again;
    big.fuzzy_search("cat", 1, &again);
    check.expect((again.visited_nodes == stats.visited_nodes) &&
                 (again.decoded_codepoints == stats.decoded_codepoints));
    const marisa::FuzzyMatch &first = result.front();
    check.expect((first.key == "cat") && (first.distance == 0));
  }

  // A nullptr stats pointer is accepted.
  check.expect(same_matches(small.fuzzy_search("cat", 1), small_result));
}

void storage(Check &check) {
  const std::vector<std::string> keys = {
      "cat",          "cats",  "猫",      "猫好",   "好",
      "good",         "god",   "goods",   "日本語",
      std::string("a\0b", 3),  std::string("g\xFF\xFEod", 5),
      "🐱🐱🐱🐱",  "🐱🐱🐱🐶", "prefixéalpha", "prefixèalpha",
  };
  const marisa::Trie trie =
      build_trie(keys, 3 | MARISA_BINARY_TAIL | MARISA_LABEL_ORDER);
  const std::vector<marisa::FuzzyMatch> before = trie.fuzzy_search("猫", 1);
  check.expect(same_matches(before, brute_force(trie, keys, "猫", 1)));

  // Key IDs agree with lookup()/reverse_lookup() before persistence.
  for (const auto &match : before) {
    marisa::Agent agent;
    agent.set_query(match.key.data(), match.key.size());
    check.expect(trie.lookup(agent) && (agent.key().id() == match.id));
    marisa::Agent back;
    back.set_query(match.id);
    trie.reverse_lookup(back);
    check.expect(std::string(back.key().ptr(), back.key().length()) ==
                 match.key);
  }

  const char *dir = std::getenv("TMPDIR");
  std::string path = (dir != nullptr) && (dir[0] != '\0') ? dir : ".";
  if (path.back() != '/') {
    path += '/';
  }
  path += "marisa_fuzzy_target_test.dic";

  trie.save(path.c_str());

  marisa::Trie loaded;
  loaded.load(path.c_str());
  check.expect(same_matches(loaded.fuzzy_search("猫", 1), before));
  check.expect(
      same_matches(loaded.fuzzy_search("prefixéalpha", 2),
                   brute_force(loaded, keys, "prefixéalpha", 2)));

  // IDs are preserved by save/load.
  check.expect(same_matches(loaded.fuzzy_search("good", 2),
                            trie.fuzzy_search("good", 2)));

  marisa::Trie mapped;
  mapped.mmap(path.c_str());
  check.expect(same_matches(mapped.fuzzy_search("猫", 1), before));
  check.expect(same_matches(mapped.fuzzy_search("🐱🐱🐱🐱", 1),
                            trie.fuzzy_search("🐱🐱🐱🐱", 1)));

  std::remove(path.c_str());
  check.expect(true);
}

}  // namespace

void register_tests(Suite &suite) {
  suite.add("unicode", unicode);
  suite.add("compressed", compressed);
  suite.add("pruning", pruning);
  suite.add("storage", storage);
}
